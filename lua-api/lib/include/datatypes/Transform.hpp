#pragma once

#include <vector>
#include <datatypes/Vector.hpp>
#include <datatypes/StructObject.hpp>
#include <datatypes/Quaternion.hpp>



namespace lua::datatypes {
   
    template <typename T1, typename T2> 
    struct Transform {
          T1 translation;
          T2 rotation;
          T1 scale3d;  


       Transform(T1 t, T2 r, T1 s) 
        : translation(t), rotation(r), scale3d(s) {}
    Transform() 
        : translation(T1(0.0f)), 
          rotation(T2(1.0f, 0.0f, 0.0f, 0.0f)), 
          scale3d(T1(1.0f)) {}
   Transform(T2 r) 
        : translation(T1(0.0f)), 
          rotation(r), 
          scale3d(T1(1.0f)) {}
   Transform(T1 t) 
        : translation(t), 
          rotation(T2(1.0f, 0.0f, 0.0f, 0.0f)), 
          scale3d(T1(1.0f)) {}
};
 
   using Transformf = Transform<Vector3f, Quaternionf>;
   using Transformd = Transform<Vector3d, Quaterniond>;
   void bind_transform_struct(sol::state_view &lua);


}
