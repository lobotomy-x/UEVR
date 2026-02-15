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


        lua.new_usertype<Quaternionf>("Quaternionf", "set", [](Quaternionf& v, float x, float y, float z, float w) {
        v.x = x;
        v.y = y;
        v.z = z;
        v.w = w; }, "x", & Quaternionf::x, "y", & Quaternionf::y, "z", & Quaternionf::z, "w", & Quaternionf::w, "X", & Quaternionf::x, "Y", & Quaternionf::y, "Z", & Quaternionf::z, "W", & Quaternionf::w, "length", [](const Quaternionf& v) {
        return glm::length(v); }, "normalize", [](Quaternionf& v) {
        v = glm::normalize(v); }, "normalized", [](const Quaternionf& v) {
        return glm::normalize(v); }, "conjugate", [](const Quaternionf& v) {
        return glm::conjugate(v); }, "inverse", [](const Quaternionf& v) {
        return glm::inverse(v); }, "identity", []() {
        return glm::identity<Quaternionf>(); }, "to_mat4", [](const Quaternionf& q) {
        return Matrix4x4f{q}; }, "to_vec4", [](const Quaternionf& v) {
        return Vector4f{v.x, v.y, v.z, v.w}; }, "dot", [](const Quaternionf& v1, const Quaternionf& v2) {
        return glm::dot(v1, v2); }, "slerp", [](const Quaternionf& v1, const Quaternionf& v2, float t) {
        return glm::slerp(v1, v2, t); }, sol::meta_function::addition, [](const Quaternionf& lhs, const Quaternionf& rhs) {
        return lhs + rhs; }, sol::meta_function::subtraction, [](const Quaternionf& lhs, const Quaternionf& rhs) {
        return lhs - rhs; }, sol::meta_function::multiplication, sol::overload([](const Quaternionf& lhs, float scalar) ->Quaternionf {
        return lhs * scalar;}, [](const Quaternionf& lhs, const Quaternionf& rhs) -> Quaternionf {
        return lhs * rhs;}, [](const Quaternionf& lhs, const Vector3f& rhs) -> Vector3f {
        return lhs * rhs;}, [](const Quaternionf& lhs, const Vector4f& rhs) -> Vector4f {
        return lhs * rhs; }), sol::meta_function::index, [](sol::this_state s, const Quaternionf& lhs, sol::object index_obj) -> sol::object {
        if (!index_obj.is<int>()) {
            return sol::make_object(s, sol::lua_nil);
        }
        const auto index = index_obj.as<int>();
        if (index >= 4) {
            return sol::make_object(s, sol::lua_nil);
        }
        return sol::make_object(s, lhs[index]); }, sol::meta_function::new_index, [](Quaternionf& lhs, int index, float rhs) {
        if (index < 4) {
            lhs[index] = rhs;
        } },
            sol::meta_function::construct, sol::constructors<Quaternionf(float, float, float, float)>());
        
        lua.new_usertype<Quaterniond>("Quaterniond", "set", [](Quaterniond& v, double x, double y, double z, double w) {
        v.x = x;
        v.y = y;
        v.z = z;
        v.w = w; }, "x", & Quaterniond::x, "y", & Quaterniond::y, "z", & Quaterniond::z, "w", & Quaterniond::w, "X", & Quaterniond::x, "Y", & Quaterniond::y, "Z", & Quaterniond::z, "W", & Quaterniond::w, "length", [](const Quaterniond& v) {
        return glm::length(v); }, "normalize", [](Quaterniond& v) {
        v = glm::normalize(v); }, "normalized", [](const Quaterniond& v) {
        return glm::normalize(v); }, "conjugate", [](const Quaterniond& v) {
        return glm::conjugate(v); }, "inverse", [](const Quaterniond& v) {
        return glm::inverse(v); }, "identity", []() {
        return glm::identity<Quaterniond>(); }, "to_vec4", [](const Quaterniond& v) {
        return Vector4d{v.x, v.y, v.z, v.w}; }, "dot", [](const Quaterniond& v1, const Quaterniond& v2) {
        return glm::dot(v1, v2); }, "slerp", [](const Quaterniond& v1, const Quaterniond& v2, double t) {
        return glm::slerp(v1, v2, t); }, sol::meta_function::addition, [](const Quaterniond& lhs, const Quaterniond& rhs) {
        return lhs + rhs; }, sol::meta_function::subtraction, [](const Quaterniond& lhs, const Quaterniond& rhs) {
        return lhs - rhs; }, sol::meta_function::multiplication, sol::overload([](const Quaterniond& lhs, double scalar) ->Quaterniond {
        return lhs * scalar;}, [](const Quaterniond& lhs, const Quaterniond& rhs) -> Quaterniond {
        return lhs * rhs;}, [](const Quaterniond& lhs, const Vector4d& rhs) -> Vector4d {
        return lhs * rhs; }), sol::meta_function::index, [](sol::this_state s, const Quaterniond& lhs, sol::object index_obj) -> sol::object {
        if (!index_obj.is<int>()) {
            return sol::make_object(s, sol::lua_nil);
        }
        const auto index = index_obj.as<int>();
        if (index >= 4) {
            return sol::make_object(s, sol::lua_nil);
        }
        return sol::make_object(s, lhs[index]); }, sol::meta_function::new_index, [](Quaterniond& lhs, int index, double rhs) {
        if (index < 4) {
            lhs[index] = rhs;
        } },
            sol::meta_function::construct, sol::constructors<Quaterniond(double, double, double, double)>());

               //"to_euler", [](const name& q) ->name2 { return utility::math::euler_angles(name4{q}); }, 
    }
}