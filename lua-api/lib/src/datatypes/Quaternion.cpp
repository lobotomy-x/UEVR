#include <datatypes/Quaternion.hpp>

namespace lua::datatypes {

// Macro for a glm::tquat<T>-like type. Generates the full set of fields, accessors, conversions
// and metamethods (incl. scalar*quat reverse multiplication and quat*vec3/quat*vec4 rotation).
#define BIND_QUATERNION_LIKE(name, datatype, vec3_t, vec4_t, mat4_t) \
    lua.new_usertype<name>(#name, \
        sol::meta_function::construct, sol::constructors<name(), name(datatype, datatype, datatype, datatype)>(), \
        "set", [](name& v, datatype x, datatype y, datatype z, datatype w) { \
            v.x = x; v.y = y; v.z = z; v.w = w; }, \
        "clone", [](const name& v) -> name { return v; }, \
        "x", &name::x, "y", &name::y, "z", &name::z, "w", &name::w, \
        "X", &name::x, "Y", &name::y, "Z", &name::z, "W", &name::w, \
        "length", [](const name& v) { return glm::length(v); }, \
        "normalize", [](name& v) { v = glm::normalize(v); }, \
        "normalized", [](const name& v) { return glm::normalize(v); }, \
        "conjugate", [](const name& v) { return glm::conjugate(v); }, \
        "inverse", [](const name& v) { return glm::inverse(v); }, \
        "identity", []() { return glm::identity<name>(); }, \
        "dot", [](const name& a, const name& b) { return glm::dot(a, b); }, \
        "slerp", [](const name& a, const name& b, datatype t) { return glm::slerp(a, b, t); }, \
        "to_mat4", [](const name& q) { return mat4_t{q}; }, \
        "to_vec4", [](const name& q) { return vec4_t{q.x, q.y, q.z, q.w}; }, \
        "rotator", [](const name& q) -> vec3_t { return glm::degrees(glm::eulerAngles(q)); }, \
        "quaternion", [](const vec3_t& r) -> name { \
            return glm::tquat<datatype>(glm::radians(vec3_t{-r.z, -r.y, -r.x})); }, \
        "rotate", [](const name& q, const vec3_t& v) -> vec3_t { return q * v; }, \
        "unrotate", [](const name& q, const vec3_t& v) -> vec3_t { return glm::inverse(q) * v; }, \
        "x_axis", [](const name& q) -> vec3_t { return q * vec3_t(1, 0, 0); }, \
        "y_axis", [](const name& q) -> vec3_t { return q * vec3_t(0, 1, 0); }, \
        "z_axis", [](const name& q) -> vec3_t { return q * vec3_t(0, 0, 1); }, \
        sol::meta_function::to_string, [](const name& v) { \
            return "(" + std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z) + "," + std::to_string(v.w) + ")"; }, \
        sol::meta_function::unary_minus, [](const name& q) { return name{-q.w, -q.x, -q.y, -q.z}; }, \
        sol::meta_function::equal_to, [](const name& a, const name& b) { return a == b; }, \
        sol::meta_function::addition, [](const name& a, const name& b) { return a + b; }, \
        sol::meta_function::subtraction, [](const name& a, const name& b) { return a - b; }, \
        sol::meta_function::multiplication, sol::overload( \
            [](const name& a, const name& b) -> name { return a * b; }, \
            [](const name& a, datatype s) -> name { return a * s; }, \
            [](datatype s, const name& a) -> name { return a * s; }, \
            [](const name& q, const vec3_t& v) -> vec3_t { return q * v; }, \
            [](const name& q, const vec4_t& v) -> vec4_t { return q * v; }))

void bind_quaternions(sol::state_view& lua) {
    BIND_QUATERNION_LIKE(Quaternionf, float, Vector3f, Vector4f, Matrix4x4f);
    BIND_QUATERNION_LIKE(Quaterniond, double, Vector3d, Vector4d, Matrix4x4d);

    // Cross-precision conversions.
    auto qf = lua["Quaternionf"];
    qf["to_quatd"] = [](const Quaternionf& q) { return Quaterniond{q.w, q.x, q.y, q.z}; };

    auto qd = lua["Quaterniond"];
    qd["to_quatf"] = [](const Quaterniond& q) {
        return Quaternionf{(float)q.w, (float)q.x, (float)q.y, (float)q.z};
    };

    // Static factory aliases. `quaternion` and `identity` are registered by
    // the macro as USERTYPE METHODS — sol stores them in the type's metatable
    // __index, which means they're only reachable via `instance:method(...)`.
    // Calling `Quaternionf.quaternion(vec3)` (dot-style, no self) fails with
    // "no matching function call" because sol tries to bind the Vector3 to
    // the implicit Quaternionf& self. Assigning here puts the same functions
    // directly on the type table so dot-style calls work — `Quaternionf.from_euler`
    // and `Quaternionf.identity` are now both static factories that take only
    // their actual args. The original member bindings stay intact for any
    // script that already happened to be using colon syntax.
    qf["from_euler"] = [](const Vector3f& r) -> Quaternionf {
        return glm::tquat<float>(glm::radians(Vector3f{-r.z, -r.y, -r.x}));
    };
    qf["identity"] = []() { return glm::identity<Quaternionf>(); };

    qd["from_euler"] = [](const Vector3d& r) -> Quaterniond {
        return glm::tquat<double>(glm::radians(Vector3d{-r.z, -r.y, -r.x}));
    };
    qd["identity"] = []() { return glm::identity<Quaterniond>(); };
}

}
