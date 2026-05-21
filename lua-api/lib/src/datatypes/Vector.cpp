    #include <datatypes/Vector.hpp>

    namespace lua::datatypes {
    // const auto& vector_components = {"x", "y", "z", "w", "pitch", "yaw", "roll", "r", "g", "b", "a"};

    /**
     * Binds all vector types (Vector2*, Vector3*, Vector4*) into the provided Sol Lua state.
     * Exposes fields, common vector operations, constructors and metamethods.
     * This function registers both float and double variants.
     */
    void bind_vectors(sol::state_view& lua) {

    #define BIND_VECTOR3_LIKE(name, datatype, name2, name3) \
                lua.new_usertype<name>(#name, \
                    "set", [](sol::object o, datatype x, datatype y, datatype z) -> sol::object { \
                        name& v = o.as<name>(); \
                        v.x = x; v.y = y; v.z = z; \
                        return o; \
                    }, \
                    "clone", [](const name& v) -> name { return v; }, \
                    "x", &name::x, \
                    "y", &name::y, \
                    "z", &name::z, \
                    "X", &name::x, \
                    "Y", &name::y, \
                    "Z", &name::z, \
                    "Pitch", &name::x, \
                    "Yaw", &name::y, \
                    "Roll", &name::z, \
                    "pitch", &name::x, \
                    "yaw", &name::y, \
                    "roll", &name::z, \
                    "R", &name::x, \
                    "G", &name::y, \
                    "B", &name::z, \
                    "r", &name::x, \
                    "g", &name::y, \
                    "b", &name::z, \
                    "dot", [](const name& v1, const name& v2) { return glm::dot(v1, v2); }, \
                    "cross", [](const name& v1, const name& v2) { return glm::cross(v1, v2); }, \
                    "length", [](const name& v) { return glm::length(v); }, \
                    "normalize", [](name& v) { v = glm::normalize(v); }, \
                    "normalized", [](const name& v) { return glm::normalize(v); }, \
                    "reflect", [](const name& v, const name& normal) { return glm::reflect(v, normal); }, \
                    "refract", [](const name& v, const name& normal, datatype eta) { return glm::refract(v, normal, eta); }, \
                    "lerp", [](const name& v1, const name& v2, datatype t) { return glm::lerp(v1, v2, t); }, \
                    sol::meta_function::addition,  sol::overload([](const name& lhs, datatype scalar) { return name{lhs.x + scalar, lhs.y + scalar}; }, \
                        [](const name& lhs, const name& rhs) { return lhs + rhs; }), \
                    sol::meta_function::subtraction, sol::overload( [](const name& lhs, datatype scalar) { return name{lhs.x - scalar, lhs.y - scalar}; }, \
                        [](const name& lhs, const name& rhs) {  return lhs - rhs; }), \
                    sol::meta_function::multiplication, sol::overload( [](const name& lhs, const name& rhs) { return name{lhs.x * rhs.x, lhs.y * rhs.y}; },  [](const name& lhs, datatype scalar) { return lhs * scalar; }), \
                    sol::meta_function::division, sol::overload([](const name& lhs, const name& rhs) { return name{lhs.x / rhs.x, lhs.y / rhs.y}; },  [](const name& lhs, datatype scalar) { return lhs / scalar; }), \
                    sol::meta_function::length, [](const name& v) { return 3; }, \
                    sol::meta_function::to_string, [](const name& v) { return "(" + std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z) + ")"; }, \
                    sol::meta_function::unary_minus, [](const name& v) {return -v; }, \
                    "to_vec4", [](const name& v) { return name2{v.x, v.y, v.z, static_cast<datatype>(0)}; }

    #define BIND_VECTOR3_LIKE_END() \
    );

        BIND_VECTOR3_LIKE(Vector3f, float, Vector4f, Vector2f), sol::meta_function::construct,
            sol::constructors<Vector3f(float, float, float)>() BIND_VECTOR3_LIKE_END();

        BIND_VECTOR3_LIKE(Vector3d, double, Vector4d, Vector2d), sol::meta_function::construct,
            sol::constructors<Vector3d(double, double, double)>() BIND_VECTOR3_LIKE_END();

    #define BIND_VECTOR4_LIKE(name, datatype, datatype2, name2, name3)                                                                                                                               \
                lua.new_usertype<name>(#name, \
                    "set", [](sol::object o, datatype x, datatype y, datatype z, datatype w) -> sol::object { \
                        name& v = o.as<name>(); \
                        v.x = x; v.y = y; v.z = z; v.w = w; \
                        return o; \
                    }, \
                    "clone", [](const name& v) -> name { return v; }, \
                    "x", &name::x, \
                    "y", &name::y, \
                    "z", &name::z, \
                    "w", &name::w, \
                    "X", &name::x, \
                    "Y", &name::y, \
                    "Z", &name::z, \
                    "W", &name::w, \
                    "Pitch", &name::x, \
                    "Yaw", &name::y, \
                    "Roll", &name::z, \
                    "pitch", &name::x, \
                    "yaw", &name::y, \
                    "roll", &name::z, \
                    "R", &name::x, \
                    "G", &name::y, \
                    "B", &name::z, \
                    "A", &name::w, \
                    "r", &name::x, \
                    "g", &name::y, \
                    "b", &name::z, \
                    "a", &name::w, \
                    "dot", [](const name& v1, const name& v2) { return glm::dot(v1, v2); }, \
                    "length", [](const name& v) { return glm::length(v); }, \
                    "normalize", [](name& v) { v = glm::normalize(v); }, \
                    "normalized", [](const name& v) { return glm::normalize(v); }, \
                    "reflect", [](const name& v, const name& normal) { return glm::reflect(v, normal); }, \
                    "refract", [](const name& v, const name& normal, datatype eta) { return glm::refract(v, normal, eta); }, \
                    "lerp", [](const name& v1, const name& v2, datatype t) { return glm::lerp(v1, v2, t); }, \
                    sol::meta_function::length, [](const name& v) { return 4; }, \
                    sol::meta_function::to_string, [](const name& v) { return "(" + std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z) + "," + std::to_string(v.w) + ")";}, \
                    sol::meta_function::unary_minus, [](const name& v) {return -v; }, \
                    sol::meta_function::addition,  sol::overload([](const name& lhs, datatype scalar) { return name{lhs.x + scalar, lhs.y + scalar}; }, \
                        [](const name& lhs, const name& rhs) { return lhs + rhs; }), \
                    sol::meta_function::subtraction, sol::overload( [](const name& lhs, datatype scalar) { return name{lhs.x - scalar, lhs.y - scalar}; }, \
                        [](const name& lhs, const name& rhs) {  return lhs - rhs; }), \
                    sol::meta_function::multiplication, sol::overload( [](const name& lhs, const name& rhs) { return name{lhs.x * rhs.x, lhs.y * rhs.y}; },  [](const name& lhs, datatype scalar) { return lhs * scalar; }), \
                    sol::meta_function::division, sol::overload([](const name& lhs, const name& rhs) { return name{lhs.x / rhs.x, lhs.y / rhs.y}; },  [](const name& lhs, datatype scalar) { return lhs / scalar; })

    #define BIND_VECTOR4_LIKE_END() \
                );

        BIND_VECTOR4_LIKE(Vector4f, float, Quaternionf, Vector3f, Vector2f),
            sol::meta_function::construct, sol::constructors<Vector4f(float, float, float, float)>() BIND_VECTOR4_LIKE_END();
        BIND_VECTOR4_LIKE(Vector4d, double, Quaterniond, Vector3d, Vector2d),
            sol::meta_function::construct, sol::constructors<Vector4d(double, double, double, double)>() BIND_VECTOR4_LIKE_END();

    #define BIND_VECTOR2_LIKE(name, datatype, name2, name3)                                                                                                  \
                lua.new_usertype<name>(#name, \
                    "clone", [](name& v) -> name { return v; }, \
                    "x", &name::x,  \
                    "y", &name::y,  \
                    "X", &name::x,  \
                    "Y", &name::y,  \
                    "dot", [](name& v1, name& v2) { return glm::dot(v1, v2); }, \
                    "length", [](name& v) { return glm::length(v); }, \
                    "normalize", [](name& v) { v = glm::normalize(v); }, \
                    "normalized", [](name& v) { return glm::normalize(v); }, \
                    sol::meta_function::length, [](const name& v) { return 2; }, \
                    sol::meta_function::to_string, [](const name& v) { return "(" + std::to_string(v.x) + "," + std::to_string(v.y) + ")";}, \
                    sol::meta_function::unary_minus, [](const name& v) {return -v; }, \
                    sol::meta_function::addition,  sol::overload([](const name& lhs, datatype scalar) { return name{lhs.x + scalar, lhs.y + scalar}; }, \
                        [](const name& lhs, const name& rhs) { return lhs + rhs; }), \
                    sol::meta_function::subtraction, sol::overload( [](const name& lhs, datatype scalar) { return name{lhs.x - scalar, lhs.y - scalar}; }, \
                        [](const name& lhs, const name& rhs) {  return lhs - rhs; }), \
                    sol::meta_function::multiplication, sol::overload( [](const name& lhs, const name& rhs) { return name{lhs.x * rhs.x, lhs.y * rhs.y}; },  [](const name& lhs, datatype scalar) { return lhs * scalar; }), \
                    sol::meta_function::division, sol::overload([](const name& lhs, const name& rhs) { return name{lhs.x / rhs.x, lhs.y / rhs.y}; },  [](const name& lhs, datatype scalar) { return lhs / scalar; })

    #define BIND_VECTOR2_LIKE_END() \
                );

        BIND_VECTOR2_LIKE(Vector2f, float, Vector3f, Vector4f),
            sol::meta_function::construct, sol::constructors<Vector2f(float, float)>() BIND_VECTOR2_LIKE_END();

        BIND_VECTOR2_LIKE(Vector2d, double, Vector3d, Vector4d),
            sol::meta_function::construct, sol::constructors<Vector2d(double, double)>() BIND_VECTOR2_LIKE_END();
    }
    }
     // namespace lua::datatypes