#include <datatypes/StructObject.hpp>
#include <datatypes/Transform.hpp>
#include "Transform.hpp"

namespace lua::datatypes {





void bind_transform_struct(sol::state_view& lua) {
    using Quaternionf = lua::datatypes::Quaternionf;
    using Quaterniond = lua::datatypes::Quaterniond;
    using Vector3f = lua::datatypes::Vector3f;
    using Vector3d = lua::datatypes::Vector3d;
    #define BIND_TRANSFORM_LIKE(name, type1, type2) \
        lua.new_usertype<name>(#name, \
            "set", [](sol::object o, type1 trans, type2 rot, type1 scale)  -> sol::object { \
                name& t = o.as<name>(); \
                t.translation = trans; \
                t.rotation = rot; \
                t.scale3d = scale; \
                return o;      \
            }   \ 
                "translation", &name::translation, \
                "rotation", &name::rotation, \
                "scale3d", &name::scale3d, \
                "translation", &name::translation,              \
                "Translation", &name::translation,                            \
                "location", &name::translation,                             \
                "Location", &name::translation,                             \
                "rotation", &name::rotation,                                \
                "Rotation", &name::rotation,                                \
                "scale3d", &name::scale3d,                                   \
                "scale", &name::scale3d,                                       \
                "Scale", &name::scale3d,                                        \
                "Scale", &name::scale3d,                                          \
                "location", &name::translation, \
                "scale", &name::scale3d, \
                "rotation", [](&name t, &type2 q) {return type2}


#define BIND_TRANSFORM_LIKE_END()  \
        );
       

    BIND_TRANSFORM_LIKE(Transformf, Vector3f, Quaternionf),
            sol::meta_function::construct, Transformf, Transformf(Vector3f, Quaternionf, Vector3f)>()
    BIND_TRANSFORM_LIKE_END();


    BIND_TRANSFORM_LIKE(Transformd, Vector3d, Quaterniond),
        sol::meta_function::construct, Transformd, Transformd(Vector3d, Quaterniond, Vector3d)>() 
    BIND_TRANSFORM_LIKE_END();

        }


}               

