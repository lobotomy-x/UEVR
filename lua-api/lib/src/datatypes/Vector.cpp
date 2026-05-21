#include <datatypes/Vector.hpp>

namespace lua::datatypes {

// Bind a Vector2-like type (glm::vec<2, T>) into the given lua state.
// Mathematical metamethods cover vec+vec, vec+scalar, scalar+vec, unary minus, and the equality/length helpers.
#define BIND_VECTOR2_LIKE(name, datatype) \
    lua.new_usertype<name>(#name, \
        sol::meta_function::construct, sol::constructors<name(), name(datatype, datatype)>(), \
        "set", [](sol::object o, datatype x, datatype y) -> sol::object { \
            name& v = o.as<name>(); v.x = x; v.y = y; return o; }, \
        "clone", [](const name& v) -> name { return v; }, \
        "x", &name::x, "y", &name::y, \
        "X", &name::x, "Y", &name::y, \
        "dot", [](const name& a, const name& b) { return glm::dot(a, b); }, \
        "length", [](const name& v) { return glm::length(v); }, \
        "length_squared", [](const name& v) { return glm::dot(v, v); }, \
        "distance", [](const name& a, const name& b) { return glm::distance(a, b); }, \
        "normalize", [](name& v) { v = glm::normalize(v); }, \
        "normalized", [](const name& v) { return glm::normalize(v); }, \
        "lerp", [](const name& a, const name& b, datatype t) { return glm::mix(a, b, t); }, \
        sol::meta_function::length, [](const name&) { return 2; }, \
        sol::meta_function::to_string, [](const name& v) { \
            return "(" + std::to_string(v.x) + "," + std::to_string(v.y) + ")"; }, \
        sol::meta_function::unary_minus, [](const name& v) { return -v; }, \
        sol::meta_function::equal_to, [](const name& a, const name& b) { return a == b; }, \
        sol::meta_function::addition, sol::overload( \
            [](const name& a, const name& b) { return a + b; }, \
            [](const name& a, datatype s) { return a + s; }, \
            [](datatype s, const name& a) { return s + a; }), \
        sol::meta_function::subtraction, sol::overload( \
            [](const name& a, const name& b) { return a - b; }, \
            [](const name& a, datatype s) { return a - s; }, \
            [](datatype s, const name& a) { return s - a; }), \
        sol::meta_function::multiplication, sol::overload( \
            [](const name& a, const name& b) { return a * b; }, \
            [](const name& a, datatype s) { return a * s; }, \
            [](datatype s, const name& a) { return s * a; }), \
        sol::meta_function::division, sol::overload( \
            [](const name& a, const name& b) { return a / b; }, \
            [](const name& a, datatype s) { return a / s; }))

// Bind a Vector3-like type. Note metamethods operate on all 3 components (the prior implementation only
// touched x/y, silently zeroing z on every arithmetic operation).
#define BIND_VECTOR3_LIKE(name, datatype) \
    lua.new_usertype<name>(#name, \
        sol::meta_function::construct, sol::constructors<name(), name(datatype, datatype, datatype)>(), \
        "set", [](sol::object o, datatype x, datatype y, datatype z) -> sol::object { \
            name& v = o.as<name>(); v.x = x; v.y = y; v.z = z; return o; }, \
        "clone", [](const name& v) -> name { return v; }, \
        "x", &name::x, "y", &name::y, "z", &name::z, \
        "X", &name::x, "Y", &name::y, "Z", &name::z, \
        "pitch", &name::x, "yaw", &name::y, "roll", &name::z, \
        "Pitch", &name::x, "Yaw", &name::y, "Roll", &name::z, \
        "r", &name::x, "g", &name::y, "b", &name::z, \
        "R", &name::x, "G", &name::y, "B", &name::z, \
        "dot", [](const name& a, const name& b) { return glm::dot(a, b); }, \
        "cross", [](const name& a, const name& b) { return glm::cross(a, b); }, \
        "length", [](const name& v) { return glm::length(v); }, \
        "length_squared", [](const name& v) { return glm::dot(v, v); }, \
        "distance", [](const name& a, const name& b) { return glm::distance(a, b); }, \
        "normalize", [](name& v) { v = glm::normalize(v); }, \
        "normalized", [](const name& v) { return glm::normalize(v); }, \
        "reflect", [](const name& v, const name& n) { return glm::reflect(v, n); }, \
        "refract", [](const name& v, const name& n, datatype eta) { return glm::refract(v, n, eta); }, \
        "lerp", [](const name& a, const name& b, datatype t) { return glm::mix(a, b, t); }, \
        sol::meta_function::length, [](const name&) { return 3; }, \
        sol::meta_function::to_string, [](const name& v) { \
            return "(" + std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z) + ")"; }, \
        sol::meta_function::unary_minus, [](const name& v) { return -v; }, \
        sol::meta_function::equal_to, [](const name& a, const name& b) { return a == b; }, \
        sol::meta_function::addition, sol::overload( \
            [](const name& a, const name& b) { return a + b; }, \
            [](const name& a, datatype s) { return a + s; }, \
            [](datatype s, const name& a) { return s + a; }), \
        sol::meta_function::subtraction, sol::overload( \
            [](const name& a, const name& b) { return a - b; }, \
            [](const name& a, datatype s) { return a - s; }, \
            [](datatype s, const name& a) { return s - a; }), \
        sol::meta_function::multiplication, sol::overload( \
            [](const name& a, const name& b) { return a * b; }, \
            [](const name& a, datatype s) { return a * s; }, \
            [](datatype s, const name& a) { return s * a; }), \
        sol::meta_function::division, sol::overload( \
            [](const name& a, const name& b) { return a / b; }, \
            [](const name& a, datatype s) { return a / s; }))

// Bind a Vector4-like type. Same comment as Vector3 - metamethods now operate on all 4 components.
#define BIND_VECTOR4_LIKE(name, datatype) \
    lua.new_usertype<name>(#name, \
        sol::meta_function::construct, sol::constructors<name(), name(datatype, datatype, datatype, datatype)>(), \
        "set", [](sol::object o, datatype x, datatype y, datatype z, datatype w) -> sol::object { \
            name& v = o.as<name>(); v.x = x; v.y = y; v.z = z; v.w = w; return o; }, \
        "clone", [](const name& v) -> name { return v; }, \
        "x", &name::x, "y", &name::y, "z", &name::z, "w", &name::w, \
        "X", &name::x, "Y", &name::y, "Z", &name::z, "W", &name::w, \
        "pitch", &name::x, "yaw", &name::y, "roll", &name::z, \
        "Pitch", &name::x, "Yaw", &name::y, "Roll", &name::z, \
        "r", &name::x, "g", &name::y, "b", &name::z, "a", &name::w, \
        "R", &name::x, "G", &name::y, "B", &name::z, "A", &name::w, \
        "dot", [](const name& a, const name& b) { return glm::dot(a, b); }, \
        "length", [](const name& v) { return glm::length(v); }, \
        "length_squared", [](const name& v) { return glm::dot(v, v); }, \
        "distance", [](const name& a, const name& b) { return glm::distance(a, b); }, \
        "normalize", [](name& v) { v = glm::normalize(v); }, \
        "normalized", [](const name& v) { return glm::normalize(v); }, \
        "reflect", [](const name& v, const name& n) { return glm::reflect(v, n); }, \
        "refract", [](const name& v, const name& n, datatype eta) { return glm::refract(v, n, eta); }, \
        "lerp", [](const name& a, const name& b, datatype t) { return glm::mix(a, b, t); }, \
        sol::meta_function::length, [](const name&) { return 4; }, \
        sol::meta_function::to_string, [](const name& v) { \
            return "(" + std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z) + "," + std::to_string(v.w) + ")"; }, \
        sol::meta_function::unary_minus, [](const name& v) { return -v; }, \
        sol::meta_function::equal_to, [](const name& a, const name& b) { return a == b; }, \
        sol::meta_function::addition, sol::overload( \
            [](const name& a, const name& b) { return a + b; }, \
            [](const name& a, datatype s) { return a + s; }, \
            [](datatype s, const name& a) { return s + a; }), \
        sol::meta_function::subtraction, sol::overload( \
            [](const name& a, const name& b) { return a - b; }, \
            [](const name& a, datatype s) { return a - s; }, \
            [](datatype s, const name& a) { return s - a; }), \
        sol::meta_function::multiplication, sol::overload( \
            [](const name& a, const name& b) { return a * b; }, \
            [](const name& a, datatype s) { return a * s; }, \
            [](datatype s, const name& a) { return s * a; }), \
        sol::meta_function::division, sol::overload( \
            [](const name& a, const name& b) { return a / b; }, \
            [](const name& a, datatype s) { return a / s; }))

void bind_vectors(sol::state_view& lua) {
    BIND_VECTOR2_LIKE(Vector2f, float);
    BIND_VECTOR2_LIKE(Vector2d, double);

    BIND_VECTOR3_LIKE(Vector3f, float);
    BIND_VECTOR3_LIKE(Vector3d, double);

    BIND_VECTOR4_LIKE(Vector4f, float);
    BIND_VECTOR4_LIKE(Vector4d, double);

    // Cross-precision and cross-arity conversions.
    // Returned through helper methods so scripts can move between glm precisions without manual reconstruction.
    auto v2f = lua["Vector2f"];
    v2f["to_vec2d"] = [](const Vector2f& v) { return Vector2d{v.x, v.y}; };
    v2f["to_vec3"] = [](const Vector2f& v) { return Vector3f{v.x, v.y, 0.0f}; };
    v2f["to_vec4"] = [](const Vector2f& v) { return Vector4f{v.x, v.y, 0.0f, 0.0f}; };

    auto v2d = lua["Vector2d"];
    v2d["to_vec2f"] = [](const Vector2d& v) { return Vector2f{(float)v.x, (float)v.y}; };
    v2d["to_vec3"] = [](const Vector2d& v) { return Vector3d{v.x, v.y, 0.0}; };
    v2d["to_vec4"] = [](const Vector2d& v) { return Vector4d{v.x, v.y, 0.0, 0.0}; };

    auto v3f = lua["Vector3f"];
    v3f["to_vec3d"] = [](const Vector3f& v) { return Vector3d{v.x, v.y, v.z}; };
    v3f["to_vec2"] = [](const Vector3f& v) { return Vector2f{v.x, v.y}; };
    v3f["to_vec4"] = [](const Vector3f& v) { return Vector4f{v.x, v.y, v.z, 0.0f}; };

    auto v3d = lua["Vector3d"];
    v3d["to_vec3f"] = [](const Vector3d& v) { return Vector3f{(float)v.x, (float)v.y, (float)v.z}; };
    v3d["to_vec2"] = [](const Vector3d& v) { return Vector2d{v.x, v.y}; };
    v3d["to_vec4"] = [](const Vector3d& v) { return Vector4d{v.x, v.y, v.z, 0.0}; };

    auto v4f = lua["Vector4f"];
    v4f["to_vec4d"] = [](const Vector4f& v) { return Vector4d{v.x, v.y, v.z, v.w}; };
    v4f["to_vec2"] = [](const Vector4f& v) { return Vector2f{v.x, v.y}; };
    v4f["to_vec3"] = [](const Vector4f& v) { return Vector3f{v.x, v.y, v.z}; };

    auto v4d = lua["Vector4d"];
    v4d["to_vec4f"] = [](const Vector4d& v) { return Vector4f{(float)v.x, (float)v.y, (float)v.z, (float)v.w}; };
    v4d["to_vec2"] = [](const Vector4d& v) { return Vector2d{v.x, v.y}; };
    v4d["to_vec3"] = [](const Vector4d& v) { return Vector3d{v.x, v.y, v.z}; };
}

}
