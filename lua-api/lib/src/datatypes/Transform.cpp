#include <datatypes/StructObject.hpp>
#include <datatypes/Transform.hpp>


namespace lua::datatypes {

void bind_transform_struct(sol::state_view& lua) {
    lua.new_usertype<Transformf>("Transformf",
        "set", [](sol::object o, const Vector3f& trans, const Quaternionf& rot, const Vector3f& scale) -> sol::object {
            Transformf& t = o.as<Transformf>();
            t.translation = trans;
            t.rotation = rot;
            t.scale3d = scale;
            return o;
        }, 
        "identity", [](Transformf t) {
            t.rotation =    Quaternionf(1.0f, 0.0f, 0.0f, 0.0f);  // glm::quat is (w,x,y,z); identity = w=1
            t.translation=  Vector3f(0.0f,0.0f,0.0f);
            t.scale3d = Vector3f(1.0f, 1.0f, 1.0f);
            return t;
        },
        "translation", &Transformf::translation,
        "Translation", &Transformf::translation,
        "location", &Transformf::translation,
        "Location", &Transformf::translation,
        "rotation", &Transformf::rotation,
        "Rotation", &Transformf::rotation,
        "scale3d", &Transformf::scale3d,
        "Scale3D", &Transformf::scale3d,
        "scale", &Transformf::scale3d,
        "Scale", &Transformf::scale3d, 
        "compose",
        [](const Transformf& A, const Transformf& B) -> Transformf {
            Transformf out = Transformf ();
            out.rotation = A.rotation * B.rotation;
            out.scale3d = A.scale3d * B.scale3d;
            out.translation = A.translation + (B.scale3d * (A.rotation * B.translation)); // order is important
            return out;
        }, 
        "invert",
        [](Transformf& t) {
            t.rotation = glm::inverse(t.rotation);
            t.scale3d = Vector3f(1.0f) / t.scale3d;
            t.translation = -((t.rotation) * ((t.translation) * (t.scale3d)));
            return t;
        },
        "inverse",
        [](const Transformf& t) -> Transformf {
            Transformf out = Transformf();
            out.rotation = glm::inverse(t.rotation);
            out.scale3d = Vector3f(1.0f) / t.scale3d;
            out.translation = -(out.rotation * (t.translation * out.scale3d));
            return out;
        },
        "relative_reversed",
        [](const Transformf& t, const Transformf& t2) -> Transformf {
            Vector3f scale = t2.scale3d / t.scale3d;
            Quaternionf q = t2.rotation * glm::inverse(t.rotation);
            Vector3f trans = t2.translation - scale * (q * t.translation);
            return Transformf(trans, q, scale);   
        },
        "relative",
        [](const Transformf& t, const Transformf& t2) -> Transformf {
            Vector3f scale = t2.scale3d / t.scale3d;
            Quaternionf q = glm::inverse(t2.rotation);
    
            return Transformf(q * (t.translation - t2.translation) * (1.0f / t2.scale3d), q * t.rotation, scale);
        },
        "to_matrix",
        [](const Transformf& t) -> Matrix4x4f {
            return glm::translate(glm::mat4(1.0f), t.translation) * glm::mat4_cast(t.rotation) * glm::scale(glm::mat4(1.0f), t.scale3d);
        },
        "from_matrix", [](const Matrix4x4f& m) -> 
            Transformf { 
            glm::vec3 scale, translation, skew;
            glm::quat rotation;
            glm::vec4 perspective;
            glm::decompose(m, scale, rotation, translation, skew, perspective);
            Transformf t = Transformf(translation, rotation, scale);
            return t;
        },
        sol::meta_function::to_string,
        [](sol::this_state s, const Transformf t) {
            Vector3f r = glm::degrees(glm::eulerAngles(t.rotation));

             return std::format("% f, % f, % f | % f, % f, % f | % f, % f, % f", t.translation.x, t.translation.y, t.translation.z, r.x, r.y, r.z,
                t.scale3d.x, t.scale3d.y, t.scale3d.z);
            },

        sol::meta_function::construct, sol::constructors<Transformf(), Transformf(Vector3f, Quaternionf, Vector3f)>());

    // Static factory with permissive coercion. The strict typed constructor
    // requires Vector3f + Quaternionf + Vector3f userdata; if any arg is
    // passed as Vector3d, a Quaterniond, or a {x,y,z} table the call falls
    // through with sol's "no matching function call" message. The script
    // ecosystem mixes precisions freely (especially since Quaternionf.from_euler
    // returns the matching glm precision based on the input vec), so handling
    // the cross-precision case here saves callers from constant manual casts.
    {
        auto coerce_vec3f = [](const sol::object& o) -> Vector3f {
            if (o.is<Vector3f>()) return o.as<Vector3f>();
            if (o.is<Vector3d>()) { auto v = o.as<Vector3d>(); return Vector3f{(float)v.x, (float)v.y, (float)v.z}; }
            if (o.is<sol::lua_table>()) {
                auto t = o.as<sol::lua_table>();
                auto get = [&](const char* k, int i) -> float {
                    sol::object x = t[k];
                    if (!x.valid() || x.is<sol::lua_nil_t>()) x = t[i];
                    return x.is<float>() ? x.as<float>() : (x.is<double>() ? (float)x.as<double>() : 0.0f);
                };
                return Vector3f{get("x", 1), get("y", 2), get("z", 3)};
            }
            throw sol::error("Transformf factory: vec3 arg must be Vector3f, Vector3d, or {x,y,z} table");
        };
        auto coerce_quat = [](const sol::object& o) -> Quaternionf {
            if (o.is<Quaternionf>()) return o.as<Quaternionf>();
            if (o.is<Quaterniond>()) { auto q = o.as<Quaterniond>(); return Quaternionf{(float)q.w, (float)q.x, (float)q.y, (float)q.z}; }
            throw sol::error("Transformf factory: quat arg must be Quaternionf or Quaterniond");
        };
        lua["Transformf"]["compose"] = [coerce_vec3f, coerce_quat](sol::object t, sol::object r, sol::object s) -> Transformf {
            return Transformf(coerce_vec3f(t), coerce_quat(r), coerce_vec3f(s));
        };
    }

    lua.new_usertype<Transformd>("Transformd",
        "set", [](sol::object o, const Vector3d& trans, const Quaterniond& rot, const Vector3d& scale) -> sol::object {
            Transformd& t = o.as<Transformd>();
            t.translation = trans;
            t.rotation = rot;
            t.scale3d = scale;
            return o;
        },
        "identity",
        [](Transformd t) {
            t.rotation = Quaterniond(1.0, 0.0, 0.0, 0.0);  // glm::dquat is (w,x,y,z); identity = w=1
            t.translation = Vector3d(0.0f, 0.0f, 0.0f);
            t.scale3d = Vector3d(1.0f, 1.0f, 1.0f);
            return t;
        },
        "translation", &Transformd::translation,
        "Translation", &Transformd::translation,
        "location", &Transformd::translation,
        "Location", &Transformd::translation,
        "rotation", &Transformd::rotation,
        "Rotation", &Transformd::rotation,
        "scale3d", &Transformd::scale3d,
        "Scale3D", &Transformd::scale3d,
        "scale", &Transformd::scale3d,
        "Scale", &Transformd::scale3d,
        "compose",
        [](const Transformd& A, const Transformd& B) -> Transformd {
            Transformd out = A;
            out.rotation = A.rotation * B.rotation;
            out.scale3d = A.scale3d * B.scale3d;
            out.translation = A.translation + (B.scale3d * (A.rotation * B.translation)); // order is important
            return out;
        },
        "invert",
        [](Transformd& t) {
            t.rotation = glm::inverse(t.rotation);
            t.scale3d = Vector3d(1.0f) / t.scale3d;
            t.translation = -(Quaterniond(t.rotation) * (Vector3d(t.translation) * Vector3d(t.scale3d)));
            return t;
        },
        "inverse",
        [](const Transformd& t) -> Transformd {
            Transformd out = Transformd();
            out.rotation = glm::inverse(t.rotation);
            out.scale3d = Vector3d(1.0f) / t.scale3d;
            out.translation = -(out.rotation * (t.translation * out.scale3d));
            return out;
        },
        "to_matrix",
        [](const Transformd& t) -> Matrix4x4d {
            return glm::translate(glm::dmat4(1.0f), t.translation) * glm::mat4_cast(t.rotation) * glm::scale(glm::dmat4 (1.0f), t.scale3d);
        },
        "from_matrix",
        [](const Matrix4x4d& m) -> Transformd {
            glm::dvec3 scale, translation, skew;
            glm::dquat rotation;
            glm::dvec4 perspective;
            glm::decompose(m, scale, rotation, translation, skew, perspective);
            Transformd t = Transformd(translation, rotation, scale);
            return t;
        },
        sol::meta_function::construct, sol::constructors<Transformd(), Transformd(Vector3d, Quaterniond, Vector3d)>());

    // Permissive compose factory for Transformd, mirrors the Transformf one.
    {
        auto coerce_vec3d = [](const sol::object& o) -> Vector3d {
            if (o.is<Vector3d>()) return o.as<Vector3d>();
            if (o.is<Vector3f>()) { auto v = o.as<Vector3f>(); return Vector3d{(double)v.x, (double)v.y, (double)v.z}; }
            if (o.is<sol::lua_table>()) {
                auto t = o.as<sol::lua_table>();
                auto get = [&](const char* k, int i) -> double {
                    sol::object x = t[k];
                    if (!x.valid() || x.is<sol::lua_nil_t>()) x = t[i];
                    return x.is<double>() ? x.as<double>() : (x.is<float>() ? (double)x.as<float>() : 0.0);
                };
                return Vector3d{get("x", 1), get("y", 2), get("z", 3)};
            }
            throw sol::error("Transformd factory: vec3 arg must be Vector3d, Vector3f, or {x,y,z} table");
        };
        auto coerce_quatd = [](const sol::object& o) -> Quaterniond {
            if (o.is<Quaterniond>()) return o.as<Quaterniond>();
            if (o.is<Quaternionf>()) { auto q = o.as<Quaternionf>(); return Quaterniond{(double)q.w, (double)q.x, (double)q.y, (double)q.z}; }
            throw sol::error("Transformd factory: quat arg must be Quaterniond or Quaternionf");
        };
        lua["Transformd"]["compose"] = [coerce_vec3d, coerce_quatd](sol::object t, sol::object r, sol::object s) -> Transformd {
            return Transformd(coerce_vec3d(t), coerce_quatd(r), coerce_vec3d(s));
        };
    }
}

}

