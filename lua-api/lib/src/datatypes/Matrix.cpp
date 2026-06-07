#include <datatypes/StructObject.hpp>
#include "datatypes/Matrix.hpp"
#include "datatypes/Transform.hpp"

namespace lua::datatypes {

// Macro for a glm::tmat<4,4,T>-like type. Generates fields, decompose helper, scalar/vec/mat
// multiplication overloads (incl. mat*vec3 sugar for transform_point/transform_vector), and
// element-wise inverse helpers.
#define BIND_MATRIX4_LIKE(name, datatype, vec3_t, vec4_t, quat_t, transform_t) \
    lua.new_usertype<name>(#name, \
        sol::meta_function::construct, sol::constructors< \
            name(), \
            name(const vec4_t&, const vec4_t&, const vec4_t&, const vec4_t&), \
            name(datatype, datatype, datatype, datatype, \
                 datatype, datatype, datatype, datatype, \
                 datatype, datatype, datatype, datatype, \
                 datatype, datatype, datatype, datatype)>(), \
        "clone", [](const name& m) -> name { return m; }, \
        "identity", []() { return glm::identity<name>(); }, \
        "to_quat", [](const name& m) -> quat_t { return glm::tquat<datatype>(m); }, \
        "inverse", [](const name& m) -> name { return glm::inverse(m); }, \
        "invert", [](name& m) { m = glm::inverse(m); }, \
        "transpose", [](const name& m) -> name { return glm::transpose(m); }, \
        "determinant", [](const name& m) -> datatype { return glm::determinant(m); }, \
        "decompose", [](const name& m) -> transform_t { \
            transform_t t{}; \
            glm::tvec3<datatype> scale, translation, skew; \
            glm::tquat<datatype> rotation; \
            glm::tvec4<datatype> perspective; \
            glm::decompose(m, scale, rotation, translation, skew, perspective); \
            t.translation = translation; \
            t.rotation = rotation; \
            t.scale3d = scale; \
            return t; \
        }, \
        "transform_point", [](const name& m, const vec3_t& p) -> vec3_t { \
            return vec3_t(m * vec4_t(p, (datatype)1)); }, \
        "transform_vector", [](const name& m, const vec3_t& v) -> vec3_t { \
            return vec3_t(m * vec4_t(v, (datatype)0)); }, \
        /* Full vec4 transform: w=1 so translation is applied AND the resulting w
           is preserved for the perspective divide (world->clip for w2s). */ \
        "transform_vector4", [](const name& m, const vec3_t& v) -> vec4_t { \
            return m * vec4_t(v, (datatype)1); }, \
        "transform_vector4w", [](const name& m, const vec4_t& v) -> vec4_t { \
            return m * v; }, \
        sol::meta_function::to_string, [](const name& m) { \
            std::string s = "{\n"; \
            for (int r = 0; r < 4; ++r) { \
                s += "  ("; \
                for (int c = 0; c < 4; ++c) { \
                    s += std::to_string(m[c][r]); \
                    if (c < 3) s += ", "; \
                } \
                s += ")\n"; \
            } \
            return s + "}"; }, \
        sol::meta_function::equal_to, [](const name& a, const name& b) { return a == b; }, \
        sol::meta_function::unary_minus, [](const name& m) { return -m; }, \
        sol::meta_function::addition, [](const name& a, const name& b) { return a + b; }, \
        sol::meta_function::subtraction, [](const name& a, const name& b) { return a - b; }, \
        sol::meta_function::multiplication, sol::overload( \
            [](const name& a, const name& b) -> name { return a * b; }, \
            [](const name& a, datatype s) -> name { return a * s; }, \
            [](datatype s, const name& a) -> name { return a * s; }, \
            [](const name& a, const vec4_t& v) -> vec4_t { return a * v; }, \
            [](const name& a, const vec3_t& v) -> vec3_t { \
                return vec3_t(a * vec4_t(v, (datatype)1)); }), \
        sol::meta_function::index, [](sol::this_state s, name& lhs, sol::object index_obj) -> sol::object { \
            if (!index_obj.is<int>()) return sol::make_object(s, sol::lua_nil); \
            const auto index = index_obj.as<int>(); \
            if (index < 0 || index >= 4) return sol::make_object(s, sol::lua_nil); \
            return sol::make_object(s, &lhs[index]); \
        }, \
        sol::meta_function::new_index, [](name& lhs, int index, const vec4_t& rhs) { \
            if (index >= 0 && index < 4) lhs[index] = rhs; \
        })

void bind_matrix_struct(sol::state_view& lua) {
    BIND_MATRIX4_LIKE(Matrix4x4f, float, Vector3f, Vector4f, Quaternionf, Transformf);
    BIND_MATRIX4_LIKE(Matrix4x4d, double, Vector3d, Vector4d, Quaterniond, Transformd);

    // Cross-precision conversions.
    auto m4f = lua["Matrix4x4f"];
    m4f["to_mat4d"] = [](const Matrix4x4f& m) -> Matrix4x4d { return Matrix4x4d(m); };

    auto m4d = lua["Matrix4x4d"];
    m4d["to_mat4f"] = [](const Matrix4x4d& m) -> Matrix4x4f { return Matrix4x4f(m); };
}

}
