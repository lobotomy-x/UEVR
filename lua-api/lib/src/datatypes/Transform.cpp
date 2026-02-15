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
            t.rotation = Quaternionf(0.0f, 0.0f, 0.0f, 1.0f);
            t.translation= Vector3f(0.0f,0.0f,0.0f);
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
        "scale", &Transformf::scale3d,
        "Scale", &Transformf::scale3d, 


        sol::meta_function::construct, sol::constructors<Transformf(Vector3f, Quaternionf, Vector3f)>());

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
            t.rotation = Quaterniond(0.0f, 0.0f, 0.0f, 1.0f);
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
        "scale", &Transformd::scale3d,
        "Scale", &Transformd::scale3d,
        sol::meta_function::construct, sol::constructors<Transformd(Vector3d, Quaterniond, Vector3d)>());
}

}

