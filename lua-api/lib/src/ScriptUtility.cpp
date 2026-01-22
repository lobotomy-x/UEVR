    #include <cstdint>
    #include <format>
    #include <string>

    #include <utility/String.hpp>

    #include <ScriptUtility.hpp>
    #include <datatypes/Quaternion.hpp>
    #include <datatypes/StructObject.hpp>
    #include <datatypes/Transform.hpp>
    #include <datatypes/Vector.hpp>

    namespace lua::utility {
    sol::object call_function(sol::this_state s, uevr::API::UObject* self, uevr::API::UFunction* fn, sol::variadic_args args);

    uevr::API::UScriptStruct* get_vector_struct() {
        static uevr::API::UScriptStruct* vector_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector");

            if (modern_class != nullptr) return modern_class;
            return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Vector");
        }();

        return vector_struct;
    }

    uevr::API::UScriptStruct* get_vector4_struct() {
        static uevr::API::UScriptStruct* vector_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector4");
            if (modern_class != nullptr)
                return modern_class;
            return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Vector4");
        }();

        return vector_struct;
    }
    // UE5 option only
    uevr::API::UScriptStruct* get_vector4d_struct() {
        static uevr::API::UScriptStruct* vector_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_classd =
                uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector4d");
            if (modern_classd != nullptr)
                return modern_classd;  
        }();
        return vector_struct;
    }
    // UE5 option only
    uevr::API::UScriptStruct* get_vector4f_struct() {
        static uevr::API::UScriptStruct* vector_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_classf =
                uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector4f");
            if (modern_classf != nullptr)
                return modern_classf;
        }();
        return vector_struct;
    }
          // UE5 option only
    uevr::API::UScriptStruct* get_vector3d_struct() {
        static uevr::API::UScriptStruct* vector_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_classd =
                uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector3d");
            if (modern_classd != nullptr)
                return modern_classd;  
        }();
        return vector_struct;
    }
    // UE5 option only
    uevr::API::UScriptStruct* get_vector3f_struct() {
        static uevr::API::UScriptStruct* vector_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_classf =
                uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector3f");
            if (modern_classf != nullptr)
                return modern_classf;
        }();
        return vector_struct;
    }

    uevr::API::UScriptStruct* get_vector2d_struct() {
        static uevr::API::UScriptStruct* vector_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector2D");
            if (modern_class != nullptr)
                return modern_class;
            return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Vector2D");
        }();

        return vector_struct;
    }


        uevr::API::UScriptStruct* get_linearcolor_struct() {
        static uevr::API::UScriptStruct* linear_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.LinearColor");
            if (modern_class != nullptr)
                return modern_class;
               return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.LinearColor");
        }();

        return linear_struct;
    }
    uevr::API::UScriptStruct* get_color_struct() {
        static uevr::API::UScriptStruct* color_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Color");
            if (modern_class != nullptr)
                return modern_class;
            return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Color");

        }();

        return color_struct;
    }

    uevr::API::UScriptStruct* get_quat_struct() {
        static uevr::API::UScriptStruct* quat_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_class =
                uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Quat");
            if (modern_class != nullptr)
                return modern_class;
            return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Quat");
        }();

        return quat_struct;
    }

    uevr::API::UScriptStruct* get_rotator_struct() {
        static uevr::API::UScriptStruct* rotator_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Rotator");
            if (modern_class != nullptr) return modern_class;
            return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Rotator");
        }();

        return rotator_struct;
    }


    uevr::API::UScriptStruct* get_hitresult_struct() {
        static uevr::API::UScriptStruct* hitresult_struct = []() -> uevr::API::UScriptStruct* {
            const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/Engine.HitResult");
            if (modern_class != nullptr)
                return modern_class;
            return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/Engine.HitResult");
        }();

        return hitresult_struct;
    }



    bool is_ue5() {
        static bool cached_result = []() {
            const auto c = get_vector_struct();
            if (c == nullptr) return false;
            return c->get_struct_size() == sizeof(glm::dvec3);
        }();

        return cached_result;
    }

    template<typename T>
    sol::object create_tarray_from_table(sol::this_state s, void* self, size_t offset, sol::table tbl) {
        auto& arr = *(uevr::API::TArray<T>*)((uintptr_t)self + offset);
        const size_t count = tbl.size();
    
        if (count == 0) {
            arr.count = 0;
            arr.capacity = 0;
            arr.data = nullptr;
            return sol::make_object(s, sol::lua_nil);
        }

        T* data = (T*)uevr::API::FMalloc::get()->malloc(sizeof(T) * count, alignof(T));
        for (size_t i = 0; i < count; ++i) {
            data[i] = tbl.get<T>(i + 1);
        }
    
        arr.count = (int32_t)count;
        arr.capacity = arr.count;
        arr.data = data;
        return sol::make_object(s, sol::lua_nil);
    }



    sol::object prop_to_object(sol::this_state s, void* self, const int32_t offset, const size_t name_hash, uevr::API::FProperty* desc, bool is_self_temporary) {
        const auto base_ptr = (uintptr_t)self + offset;
    
        switch (name_hash) {
        case L"BoolProperty"_fnv:
            return sol::make_object(s, ((uevr::API::FBoolProperty*)desc)->get_value_from_object(self));
        case L"FloatProperty"_fnv:
            return sol::make_object(s, *(float*)base_ptr);
        case L"DoubleProperty"_fnv:
            return sol::make_object(s, *(double*)base_ptr);
        case L"ByteProperty"_fnv:
            return sol::make_object(s, *(uint8_t*)base_ptr);
        case L"Int8Property"_fnv:
            return sol::make_object(s, *(int8_t*)base_ptr);
        case L"Int16Property"_fnv:
            return sol::make_object(s, *(int16_t*)base_ptr);
        case L"UInt16Property"_fnv:
            return sol::make_object(s, *(uint16_t*)base_ptr);
        case L"IntProperty"_fnv:
            return sol::make_object(s, *(int32_t*)base_ptr);
        case L"UIntProperty"_fnv:
        case L"UInt32Property"_fnv:
            return sol::make_object(s, *(uint32_t*)base_ptr);
        case L"UInt64Property"_fnv:
            return sol::make_object(s, *(uint64_t*)base_ptr);
        case L"Int64Property"_fnv:
            return sol::make_object(s, *(int64_t*)base_ptr);
        case L"EnumProperty"_fnv: {
            const auto ep = (uevr::API::FEnumProperty*)desc;
            const auto np = ep->get_underlying_prop();
            if (np == nullptr) return sol::make_object(s, sol::lua_nil);
            const auto np_c = np->get_class();
            if (np_c == nullptr) return sol::make_object(s, sol::lua_nil);
            const auto np_name_hash = ::utility::hash(np_c->get_fname()->to_string());
            switch (np_name_hash) {
            case L"FloatProperty"_fnv: return sol::make_object(s, *(float*)base_ptr);
            case L"DoubleProperty"_fnv: return sol::make_object(s, *(double*)base_ptr);
            case L"ByteProperty"_fnv: return sol::make_object(s, *(uint8_t*)base_ptr);
            case L"Int8Property"_fnv: return sol::make_object(s, *(int8_t*)base_ptr);
            case L"Int16Property"_fnv: return sol::make_object(s, *(int16_t*)base_ptr);
            case L"UInt16Property"_fnv: return sol::make_object(s, *(uint16_t*)base_ptr);
            case L"IntProperty"_fnv: return sol::make_object(s, *(int32_t*)base_ptr);
            case L"UIntProperty"_fnv:
            case L"UInt32Property"_fnv: return sol::make_object(s, *(uint32_t*)base_ptr);
            case L"UInt64Property"_fnv: return sol::make_object(s, *(uint64_t*)base_ptr);
            case L"Int64Property"_fnv: return sol::make_object(s, *(int64_t*)base_ptr);
            default: return sol::make_object(s, sol::lua_nil);
            }
        }
        case L"NameProperty"_fnv:
            return sol::make_object(s, *(uevr::API::FName*)base_ptr);
        case L"StrProperty"_fnv: {
            using FString = uevr::API::TArray<wchar_t>;
            const auto& str = *(FString*)base_ptr;
            if (str.data == nullptr || str.count == 0) {
                return sol::make_object(s, std::string(""));
            }
            return sol::make_object(s, std::wstring(str.data, str.count));
        }
        case L"InterfaceProperty"_fnv:
        case L"ObjectProperty"_fnv: {
            auto obj = *(uevr::API::UObject**)base_ptr;
            if (obj == nullptr) return sol::make_object(s, sol::lua_nil);
            return sol::make_object(s, obj);
        }
        case L"ClassProperty"_fnv: {
            auto cls = *(uevr::API::UClass**)base_ptr;
            if (cls == nullptr) return sol::make_object(s, sol::lua_nil);
            return sol::make_object(s, cls);
        }
        case L"Function"_fnv:
            return sol::make_object(s, (uevr::API::UFunction*)desc);
        case L"StructProperty"_fnv: {
            const auto struct_data = (void*)base_ptr;
            const auto struct_desc = ((uevr::API::FStructProperty*)desc)->get_struct();
            if (struct_desc == nullptr) return sol::make_object(s, sol::lua_nil);
            if (struct_desc == get_linearcolor_struct()) {
                 if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Vector4f*)struct_data);
                    return sol::make_object(s, (lua::datatypes::Vector4f*)struct_data);
                }
          if (is_ue5()) {
                    if (struct_desc == get_vector_struct() || struct_desc == get_rotator_struct() || struct_desc == get_vector3d_struct()) {
                         if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Vector3d*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Vector3d*)struct_data);
                        }
                   else if (struct_desc == get_vector3f_struct()) {
                         if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Vector3f*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Vector3f*)struct_data);
                        }
                   else if (struct_desc == get_vector4_struct() || struct_desc == get_vector4d_struct()) {
                         if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Vector4d*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Vector4d*)struct_data);
                        }
        
                    else if (struct_desc == get_vector4f_struct()) {
                         if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Vector4f*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Vector4f*)struct_data);
                        }
                    else if (struct_desc == get_vector2d_struct()) {
                         if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Vector2d*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Vector2d*)struct_data);
                        }
                else if (struct_desc == get_quat_struct) {
                         if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Quaterniond*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Quaterniond*)struct_data);
                        }
                else if (struct_desc == get_transform_struct) {
                            if (is_self_temporary)
                                return    sol::make_object(s, *(lua::datatypes::Transformd*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Transformd*)struct_data);
                    }
              }
    else {
                       if (struct_desc == get_vector_struct() || struct_desc == get_rotator_struct()) {
                         if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Vector3f*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Vector3f*)struct_data);
                        }
               
                   else if (struct_desc == get_vector4_struct()) {
                         if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Vector4f*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Vector4f*)struct_data);
                        }
        
             
                    else if (struct_desc == get_vector2d_struct()) {
                         if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Vector2f*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Vector2f*)struct_data);
                        }
                else if (struct_desc == get_quat_struct) {
                         if (is_self_temporary) return sol::make_object(s, *(lua::datatypes::Quaternionf*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Quaternionf*)struct_data);
                        }
                else if (struct_desc == get_transform_struct) {
                            if (is_self_temporary)
                                return    sol::make_object(s, *(lua::datatypes::Transformf*)struct_data);
                            return sol::make_object(s, (lua::datatypes::Transformf*)struct_data);
                    }
        }
            if (is_self_temporary) {
                auto new_object = std::make_unique<lua::datatypes::StructObject>(struct_desc);
                memcpy(new_object->object, struct_data, new_object->created_object.size());
                return sol::make_object(s, std::move(new_object));
            }
            {
                auto new_object = std::make_unique<lua::datatypes::StructObject>(struct_data, struct_desc);
                return sol::make_object(s, std::move(new_object));
            }
        }
        case L"ArrayProperty"_fnv: {
            const auto inner_prop = ((uevr::API::FArrayProperty*)desc)->get_inner();
            if (inner_prop == nullptr) return sol::make_object(s, sol::lua_nil);
            const auto inner_c = inner_prop->get_class();
            if (inner_c == nullptr) return sol::make_object(s, sol::lua_nil);
            const auto inner_name_hash = ::utility::hash(inner_c->get_fname()->to_string());
            switch (inner_name_hash) {
            case L"FloatProperty"_fnv:
                return tarray_to_table<float>(s, *(uevr::API::TArray<float>*)base_ptr, 0, inner_name_hash);
            case L"DoubleProperty"_fnv:
                return tarray_to_table<double>(s, *(uevr::API::TArray<double>*)base_ptr, 0, inner_name_hash);
            case L"ByteProperty"_fnv:
                return tarray_to_table<uint8_t>(s, *(uevr::API::TArray<uint8_t>*)base_ptr, 0, inner_name_hash);
            case L"Int8Property"_fnv:
                return tarray_to_table<int8_t>(s, *(uevr::API::TArray<int8_t>*)base_ptr, 0, inner_name_hash);
            case L"Int16Property"_fnv:
                return tarray_to_table<int16_t>(s, *(uevr::API::TArray<int16_t>*)base_ptr, 0, inner_name_hash);
            case L"UInt16Property"_fnv:
                return tarray_to_table<uint16_t>(s, *(uevr::API::TArray<uint16_t>*)base_ptr, 0, inner_name_hash);
            case L"IntProperty"_fnv:
                return tarray_to_table<int32_t>(s, *(uevr::API::TArray<int32_t>*)base_ptr, 0, inner_name_hash);
            case L"UIntProperty"_fnv:
            case L"UInt32Property"_fnv:
                return tarray_to_table<uint32_t>(s, *(uevr::API::TArray<uint32_t>*)base_ptr, 0, inner_name_hash);
            case L"UInt64Property"_fnv:
                return tarray_to_table<uint64_t>(s, *(uevr::API::TArray<uint64_t>*)base_ptr, 0, inner_name_hash);
            case L"Int64Property"_fnv:
                return tarray_to_table<int64_t>(s, *(uevr::API::TArray<int64_t>*)base_ptr, 0, inner_name_hash);
            case L"NameProperty"_fnv:
                return tarray_to_table<uevr::API::FName>(s, *(uevr::API::TArray<uevr::API::FName>*)base_ptr, 0, inner_name_hash);
            case L"StrProperty"_fnv: {
                using FString = uevr::API::TArray<wchar_t>;
                return tarray_to_table<FString>(s, *(uevr::API::TArray<FString>*)base_ptr, 0, inner_name_hash);
            }
            case L"InterfaceProperty"_fnv:
            case L"ObjectProperty"_fnv:
                return tarray_to_table<uevr::API::UObject*>(s, *(uevr::API::TArray<uevr::API::UObject*>*)base_ptr, 0, inner_name_hash);
            case L"WeakObjectProperty"_fnv:
                return tarray_to_table<uevr::API::FUObjectArray::FUObjectItem>(s, *(uevr::API::TArray<uevr::API::FUObjectArray::FUObjectItem>*)base_ptr, 0, inner_name_hash);
            case L"ClassProperty"_fnv:
                return tarray_to_table<uevr::API::UClass*>(s, *(uevr::API::TArray<uevr::API::UClass*>*)base_ptr, 0, inner_name_hash);
            case L"StructProperty"_fnv:
                return tarray_to_table<uevr::API::UStruct*>(s, *(uevr::API::TArray<uevr::API::UStruct*>*)base_ptr, 0, inner_name_hash);
            default:
                return sol::make_object(s, sol::lua_nil);
            }
        }
        }

        return sol::make_object(s, sol::lua_nil);
    }

    sol::object prop_to_object(sol::this_state s, void* self, uevr::API::FProperty* desc, bool is_self_temporary) {
        const auto propc = desc->get_class();
        if (propc == nullptr) {
            return sol::make_object(s, sol::lua_nil);
        }

        const auto name_hash = ::utility::hash(propc->get_fname()->to_string());
        const auto offset = desc->get_offset();
        return prop_to_object(s, self, offset, name_hash, desc, is_self_temporary);
    }

    sol::object prop_to_object(sol::this_state s, void* self, uevr::API::UStruct* c, const std::wstring& name) {
        const auto desc = c->find_property(name.c_str());
        if (desc == nullptr) {
            if (auto fn = c->find_function(name.c_str()); fn != nullptr) {
                return sol::make_object(s, fn);
            }
            return sol::make_object(s, sol::lua_nil);
        }

        return prop_to_object(s, self, desc);
    }

    sol::object prop_to_object(sol::this_state s, uevr::API::UObject* self, const std::wstring& name) {
        const auto c = self->get_class();
        if (c == nullptr) {
            return sol::make_object(s, sol::lua_nil);
        }

        return prop_to_object(s, self, c, name);
    }

    void set_property(sol::this_state s, void* self, uevr::API::UStruct* c, const std::wstring& name, sol::object value, std::vector<std::unique_ptr<wchar_t[]>>* dynamic_strings) {
        const auto desc = c->find_property(name.c_str());
        if (desc == nullptr) {
            throw sol::error(std::format("[set_property] Property '{}' not found", ::utility::narrow(name)));
        }

        set_property(s, self, c, desc, value, dynamic_strings);
    }

    void set_property(sol::this_state s, void* self, uevr::API::UStruct* owner_c, uevr::API::FProperty* desc, sol::object value, std::vector<std::unique_ptr<wchar_t[]>>* dynamic_strings) {
        const auto propc = desc->get_class();
        if (propc == nullptr) {
            throw sol::error(std::format("[set_property] Property '{}' has no class", ::utility::narrow(desc->get_fname()->to_string())));
        }

        const auto name_hash = ::utility::hash(propc->get_fname()->to_string());
        const auto offset = desc->get_offset();
        const auto base_ptr = (uintptr_t)self + offset;

        switch (name_hash) {
        case L"BoolProperty"_fnv:
            ((uevr::API::FBoolProperty*)desc)->set_value_in_object(self, value.as<bool>());
            return;
        case L"FloatProperty"_fnv:
            *(float*)base_ptr = value.as<float>();
            return;
        case L"DoubleProperty"_fnv:
            *(double*)base_ptr = value.as<double>();
            return;
        case L"ByteProperty"_fnv:
            *(uint8_t*)base_ptr = value.as<uint8_t>();
            return;
        case L"Int8Property"_fnv:
            *(int8_t*)base_ptr = value.as<int8_t>();
            return;
        case L"Int16Property"_fnv:
            *(int16_t*)base_ptr = value.as<int16_t>();
            return;
        case L"UInt16Property"_fnv:
            *(uint16_t*)base_ptr = value.as<uint16_t>();
            return;
        case L"IntProperty"_fnv:
            *(int32_t*)base_ptr = value.as<int32_t>();
            return;
        case L"UIntProperty"_fnv:
        case L"UInt32Property"_fnv:
            *(uint32_t*)base_ptr = value.as<uint32_t>();
            return;
        case L"UInt64Property"_fnv:
            *(uint64_t*)base_ptr = value.as<uint64_t>();
            return;
        case L"Int64Property"_fnv:
            *(int64_t*)base_ptr = value.as<int64_t>();
            return;
        case L"EnumProperty"_fnv: {
            const auto ep = (uevr::API::FEnumProperty*)desc;
            const auto np = ep->get_underlying_prop();
            if (np == nullptr) {
                throw sol::error("Enum property has no underlying property");
            }

            const auto np_c = np->get_class();
            if (np_c == nullptr) {
                throw sol::error("Enum property's underlying property has no class");
            }

            const auto np_name_hash = ::utility::hash(np_c->get_fname()->to_string());
            switch (np_name_hash) {
            case L"FloatProperty"_fnv:
                *(float*)base_ptr = value.as<float>();
                return;
            case L"DoubleProperty"_fnv:
                *(double*)base_ptr = value.as<double>();
                return;
            case L"ByteProperty"_fnv:
                *(uint8_t*)base_ptr = value.as<uint8_t>();
                return;
            case L"Int8Property"_fnv:
                *(int8_t*)base_ptr = value.as<int8_t>();
                return;
            case L"Int16Property"_fnv:
                *(int16_t*)base_ptr = value.as<int16_t>();
                return;
            case L"UInt16Property"_fnv:
                *(uint16_t*)base_ptr = value.as<uint16_t>();
                return;
            case L"IntProperty"_fnv:
                *(int32_t*)base_ptr = value.as<int32_t>();
                return;
            case L"UIntProperty"_fnv:
            case L"UInt32Property"_fnv:
                *(uint32_t*)base_ptr = value.as<uint32_t>();
                return;
            case L"UInt64Property"_fnv:
                *(uint64_t*)base_ptr = value.as<uint64_t>();
                return;
            case L"Int64Property"_fnv:
                *(int64_t*)base_ptr = value.as<int64_t>();
                return;
            };

            throw sol::error("Could not set enum property");
        }
        case L"NameProperty"_fnv:
            if (value.is<std::string>()) {
                *(uevr::API::FName*)base_ptr = uevr::API::FName{::utility::widen(value.as<std::string>())};
            } else if (value.is<std::wstring>()) {
                *(uevr::API::FName*)base_ptr = uevr::API::FName{value.as<std::wstring>()};
            } else if (value.is<uevr::API::FName>()) {
                *(uevr::API::FName*)base_ptr = value.as<uevr::API::FName>();
            } else {
                throw sol::error("Invalid argument type for FName");
            }
            return;
        case L"InterfaceProperty"_fnv:
        case L"ObjectProperty"_fnv:
            *(uevr::API::UObject**)base_ptr = value.as<uevr::API::UObject*>();
            return;
        case L"ClassProperty"_fnv:
            *(uevr::API::UClass**)base_ptr = value.as<uevr::API::UClass*>();
            return;
        case L"ArrayProperty"_fnv: {
            if (!value.is<sol::lua_table>()) {
                throw sol::error("Setting TArray from non-table is not implemented");
            }

            const auto inner_prop = ((uevr::API::FArrayProperty*)desc)->get_inner();
            if (inner_prop == nullptr) throw sol::error("Array property has no inner property");
            const auto inner_c = inner_prop->get_class();
            if (inner_c == nullptr) throw sol::error("Array inner property has no class");
            const auto inner_name_hash = ::utility::hash(inner_c->get_fname()->to_string());

            auto tbl = value.as<sol::table>();

            switch (inner_name_hash) {
            case L"FloatProperty"_fnv:
                return create_tarray_from_table<float>(s, self, offset, tbl);
            case L"DoubleProperty"_fnv:
                return create_tarray_from_table<double>(s, self, offset, tbl);
            case L"ByteProperty"_fnv:
                return create_tarray_from_table<uint8_t>(s, self, offset, tbl);
            case L"Int8Property"_fnv:
                return create_tarray_from_table<int8_t>(s, self, offset, tbl);
            case L"Int16Property"_fnv:
                return create_tarray_from_table<int16_t>(s, self, offset, tbl);
            case L"UInt16Property"_fnv:
                return create_tarray_from_table<uint16_t>(s, self, offset, tbl);
            case L"IntProperty"_fnv:
                return create_tarray_from_table<int32_t>(s, self, offset, tbl);
            case L"UIntProperty"_fnv:
            case L"UInt32Property"_fnv:
                return create_tarray_from_table<uint32_t>(s, self, offset, tbl);
            case L"UInt64Property"_fnv:
                return create_tarray_from_table<uint64_t>(s, self, offset, tbl);
            case L"Int64Property"_fnv:
                return create_tarray_from_table<int64_t>(s, self, offset, tbl);
            case L"InterfaceProperty"_fnv:
            case L"ClassProperty"_fnv:
            case L"ObjectProperty"_fnv:
                return create_tarray_from_table<uevr::API::UObject*>(s, self, offset, tbl);
            default:
                throw sol::error("Setting TArray for this element type is not implemented");
            }
        }
        case L"StrProperty"_fnv: {
            using FString = uevr::API::TArray<wchar_t>;
            auto& fstr = *(FString*)base_ptr;
        
            std::wstring src;
            if (value.is<std::wstring>()) {
                src = value.as<std::wstring>();
            } else if (value.is<std::string>()) {
                src = ::utility::widen(value.as<std::string>());
            } else if (value.is<wchar_t*>()) {
                src = std::wstring{value.as<wchar_t*>()};
            } else {
                throw sol::error("Invalid argument type for FString");
            }

            const auto str_size = (src.size() + 1) * sizeof(wchar_t);
        
            if (dynamic_strings != nullptr) {
                auto buffer = std::make_unique<wchar_t[]>(src.size() + 1);
                std::copy(src.begin(), src.end(), buffer.get());
                buffer[src.size()] = L'\0';
                fstr.count = src.size() + 1;
                fstr.capacity = fstr.count;
                fstr.data = buffer.get();
                dynamic_strings->push_back(std::move(buffer));
            } else {
                auto mem = uevr::API::FMalloc::get()->malloc(str_size, alignof(wchar_t));
                memcpy(mem, src.c_str(), str_size);
                fstr.count = src.size() + 1;
                fstr.capacity = fstr.count;
                fstr.data = (wchar_t*)mem;
            }
            return;
        }
        case L"StructProperty"_fnv: {
            const auto struct_desc = ((uevr::API::FStructProperty*)desc)->get_struct();
            if (struct_desc == nullptr) {
                throw sol::error("Struct property has no struct");
            }

            if (value.is<sol::lua_table>()) {
                for (const auto& [key, val] : value.as<sol::table>()) {
                    if (!key.is<std::wstring>()) {
                        throw sol::error("Invalid key type for struct property (expected string)");
                    }

                    const auto prop = struct_desc->find_property(key.as<std::wstring>().c_str());
                    if (prop == nullptr) {
                        throw sol::error(std::format("Struct property '{}' not found in {}", ::utility::narrow(key.as<std::wstring>()), ::utility::narrow(struct_desc->get_fname()->to_string())));
                    }

                    set_property(s, (void*)base_ptr, struct_desc, prop, val, dynamic_strings);
                }
            } else if (value.is<lua::datatypes::StructObject>()) {
                const auto arg = value.as<lua::datatypes::StructObject>();
                if (arg.desc != struct_desc) {
                    if (arg.desc != nullptr) {
                        throw sol::error(std::format("Invalid struct type for struct property (expected {}, got {})", ::utility::narrow(struct_desc->get_fname()->to_string()), ::utility::narrow(arg.desc->get_fname()->to_string())));
                    } else {
                        throw sol::error(std::format("Invalid struct type for struct property (expected {})", ::utility::narrow(struct_desc->get_fname()->to_string())));
                    }
                }
                memcpy((void*)base_ptr, arg.object, struct_desc->get_struct_size());
            } 

          if (struct_desc == get_linearcolor_struct()) {
                 if (value.is<lua::datatypes::Vector4f>())   
                    *(lua::datatypes::Vector4f*)base_ptr = value.as<lua::datatypes::Vector4f>();
                }
          if (is_ue5()) {
                    if (struct_desc == get_vector_struct() || struct_desc == get_rotator_struct() || struct_desc == get_vector3d_struct()) {
                         if (value.is<lua::datatypes::Vector3d>()) 
                              *(lua::datatypes::Vector3d*)base_ptr = value.as<lua::datatypes::Vector3d>();
                        }
                   else if (struct_desc == get_vector3f_struct()) {
                         if (value.is<lua::datatypes::Vector3f>())   *(lua::datatypes::Vector3f*)base_ptr = value.as<lua::datatypes::Vector3f>();
                        }
                   else if (struct_desc == get_vector4_struct() || struct_desc == get_vector4d_struct()) {
                         if (value.is<lua::datatypes::Vector4d>())   *(lua::datatypes::Vector4d*)base_ptr = value.as<lua::datatypes::Vector4d>();
                        }
        
               else if (struct_desc == get_vector4f_struct() ) {
                         if (value.is<lua::datatypes::Vector4f>())   *(lua::datatypes::Vector4f*)base_ptr = value.as<lua::datatypes::Vector4f>();
                        }
                    else if (struct_desc == get_vector2d_struct()) {
                         if (value.is<lua::datatypes::Vector2d>())   *(lua::datatypes::Vector2d*)base_ptr = value.as<lua::datatypes::Vector2d>();
                        }
                else if (struct_desc == get_quat_struct) {
                         if (value.is<lua::datatypes::Quaterniond>())   *(lua::datatypes::Quaterniond*)base_ptr = value.as<lua::datatypes::Quaterniond>();
                        }
                else if (struct_desc == get_transform_struct) {
                            if (value.is<lua::datatypes::Transformd>())
                                  *(lua::datatypes::Transformd*)base_ptr = value.as<lua::datatypes::>();
                    }
              }
        else if (struct_desc == get_vector_struct() || struct_desc == get_rotator_struct()) {
                    if (value.is<lua::datatypes::)   *(lua::datatypes::*)base_ptr = value.as<lua::datatypes::>();Vector3f*)struct_data);
                }
               
        else if (struct_desc == get_vector4_struct()) {
                if (value.is<lua::datatypes::)   *(lua::datatypes::*)base_ptr = value.as<lua::datatypes::>();Vector4f*)struct_data);
            }        
        else if (struct_desc == get_vector2d_struct()) {
                if (value.is<lua::datatypes::)   *(lua::datatypes::*)base_ptr = value.as<lua::datatypes::>();Vector2f*)struct_data);
            }
        else if (struct_desc == get_quat_struct) {
                    if (value.is<lua::datatypes::)   *(lua::datatypes::*)base_ptr = value.as<lua::datatypes::>();Quaternionf*)struct_data);
                }
        else if (struct_desc == get_transform_struct) {
            if (value.is<lua::datatypes::Transformf>())
                    *(lua::datatypes::Transformf*)base_ptr = value.as<lua::datatypes::Transformf>();
            }

        else {
                    throw sol::error("Invalid argument type for struct property");
                }
                return;
            }
        }
    }

    void set_property(sol::this_state s, uevr::API::UObject* self, const std::wstring& name, sol::object value, std::vector<std::unique_ptr<wchar_t[]>>* dynamic_strings) {
        const auto c = self->get_class();
        if (c == nullptr) {
            throw sol::error("[set_property] Object has no class");
        }

        set_property(s, self, c, name, value, dynamic_strings);
    }


sol::object call_function(sol::this_state s, uevr::API::UObject* self, uevr::API::UFunction* fn, sol::variadic_args args) {
    const auto fn_args = fn->get_child_properties();

    if (fn_args == nullptr) {
        fn->call(self, nullptr);
        const auto& arr = sol::make_object(s, sol::lua_nil);
    }

    std::vector<uint8_t> params{};
    size_t args_index{0};

    const auto ps = fn->get_properties_size();
    const auto ma = fn->get_min_alignment();

    if (ma > 1) {
        params.resize(((ps + ma - 1) / ma) * ma);
    } else {
        params.resize(ps);
    }

    uevr::API::FProperty* const auto& arr =_prop{nullptr};
    bool ret_is_bool{false};
    bool ret_is_array{false};

    std::vector<void*> dynamic_data{};
    std::vector<std::unique_ptr<wchar_t[]>> dynamic_strings{};
    std::vector<std::vector<uevr::API::UObject*>> dynamic_object_arrays{};
    std::unordered_map<uevr::API::FProperty*, size_t> prop_to_arg_index{}; // For out parameters

    for (auto arg_desc = fn_args; arg_desc != nullptr; arg_desc = arg_desc->get_next()) {
        const auto arg_c = arg_desc->get_class();

        if (arg_c == nullptr) {
            continue;
        }

        const auto arg_c_name = arg_c->get_fname()->to_string();

        if (!arg_c_name.contains(L"Property")) {
            continue;
        }

        const auto prop_desc = (uevr::API::FProperty*)arg_desc;

        if (!prop_desc->is_param()) {
            continue;
        }

        if (prop_desc->is_const auto& arr =_param()) {
            const auto& arr =_prop = prop_desc;

            if (arg_c_name == L"BoolProperty") {
                ret_is_bool = true;
            } else if (arg_c_name == L"ArrayProperty") {
                ret_is_array = true;
            }

            continue;
        } else if (prop_desc->is_out_param()) {
            prop_to_arg_index[prop_desc] = args_index;
        }

        const auto arg_hash = ::utility::hash(arg_c_name);
        const auto offset = prop_desc->get_offset();

        if (arg_hash == L"StrProperty"_fnv) {
            const auto arg_obj = args[args_index++];
            using FString = uevr::API::TArray<wchar_t>;

            auto& fstr = *(FString*)&params[offset];

            if (arg_obj.is<std::wstring>()) {
                const auto src = arg_obj.as<std::wstring>();
                auto buffer = std::make_unique<wchar_t[]>(src.size() + 1);
                std::copy(src.begin(), src.end(), buffer.get());
                buffer[src.size()] = L'\0';

                fstr.count = src.size() + 1;
                fstr.capacity = fstr.count;
                fstr.data = buffer.get();

                dynamic_strings.push_back(std::move(buffer));
            } else if (arg_obj.is<std::string>()) {
                const auto src = ::utility::widen(arg_obj.as<std::string>());
                auto buffer = std::make_unique<wchar_t[]>(src.size() + 1);
                std::copy(src.begin(), src.end(), buffer.get());
                buffer[src.size()] = L'\0';

                fstr.count = src.size() + 1;
                fstr.capacity = fstr.count;
                fstr.data = buffer.get();

                dynamic_strings.push_back(std::move(buffer));
            } else if (arg_obj.is<wchar_t*>()) {
                const auto src = std::wstring_view{arg_obj.as<wchar_t*>()};

                auto buffer = std::make_unique<wchar_t[]>(src.size() + 1);
                std::copy(src.begin(), src.end(), buffer.get());
                buffer[src.size()] = L'\0';

                fstr.count = src.size() + 1;
                fstr.capacity = fstr.count;
                fstr.data = buffer.get();

                dynamic_strings.push_back(std::move(buffer));
            } else {
                throw sol::error("Invalid argument type for FString");
            }
        } 
        else if (arg_hash == L"ArrayProperty"_fnv) {
            const auto inner_prop = ((uevr::API::FArrayProperty*)prop_desc)->get_inner();

            if (inner_prop == nullptr) {
                continue;
            }

            const auto inner_c = inner_prop->get_class();

            if (inner_c == nullptr) {
                continue;
            }

            const auto inner_name_hash = ::utility::hash(inner_c->get_fname()->to_string());
            const auto arg_obj = args[args_index++];
            if (arg_obj.is<sol::lua_table>()) {
            const auto arg_table = arg_obj.as<sol::lua_table>();
                    switch (inner_name_hash) {
                            case L"FloatProperty"_fnv: {
                              const auto& arr = *(uevr::API::TArray<float*>*)&params[offset];
                               auto dynamic_arr = dynamic_data.emplace_back();
                               dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                               arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"DoubleProperty"_fnv: {
                                const auto& arr =  *(uevr::API::TArray<double*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();


                            }
                            case L"ByteProperty"_fnv: {
                                const auto& arr =  *(uevr::API::TArray<uint8_t*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"Int8Property"_fnv: {
                                const auto& arr =  *(uevr::API::TArray<int8_t*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"Int16Property"_fnv: {
                                const auto& arr =  *(uevr::API::TArray<int16_t*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"UInt16Property"_fnv: {
                                const auto& arr =  *(uevr::API::TArray<uint16_t*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"IntProperty"_fnv: {
                                const auto& arr =  *(uevr::API::TArray<int32_t*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"UIntProperty"_fnv:
                            case L"UInt32Property"_fnv: {
                                const auto& arr =  *(uevr::API::TArray<uint32_t*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"UInt64Property"_fnv: {
                                const auto& arr =  *(uevr::API::TArray<uint64_t*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"Int64Property"_fnv: {
                                const auto& arr =  *(uevr::API::TArray<int64_t*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"NameProperty"_fnv: {
                                const auto& arr =  *(uevr::API::TArray<uevr::API::FName*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"InterfaceProperty"_fnv:
                            case L"ClassProperty"_fnv:
                            case L"ObjectProperty"_fnv: { 

                                auto& arr = *(uevr::API::TArray<uevr::API::UObject*>*)&params[offset];
                                auto dynamic_arr = dynamic_object_arrays.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();

                            }
                            case L"StructProperty"_fnv: {

                                auto& arr = *(uevr::API::TArray<uevr::API::UStruct*>*)&params[offset];
                                auto dynamic_arr = dynamic_data.emplace_back();
                                dynamic_arr.resize(arg_table.size());

                                for (size_t i = 0; i < arg_table.size(); ++i) {
                                    dynamic_arr[i] = arg_table[i + 1];
                                }
                                arr.count = (int32_t)dynamic_arr.size();
                                arr.capacity = arr.count;
                                arr.data = dynamic_arr.data();
                            }
                        }
                    }
      }
           
         else {
            if (prop_desc->is_out_param() && args[args_index].is<sol::lua_table>()) {
                args_index++;
                continue;
            }

            set_property(s, params.data(), fn, prop_desc, args[args_index++], &dynamic_strings);
        }
    }

    fn->call(self, params.data());

    // Handle out parameters
    for (const auto& [prop, arg_index] : prop_to_arg_index) {
        const auto prop_c = prop->get_class();
        const auto prop_name_hash = ::utility::hash(prop_c->get_fname()->to_string());

        if (args[arg_index].is<lua::datatypes::StructObject>()) {
            if (prop_name_hash != L"StructProperty"_fnv) {
                throw sol::error("Invalid struct type for out parameter");
            }

            const auto structprop = (uevr::API::FStructProperty*)prop;

            auto& arg = args[arg_index].as<lua::datatypes::StructObject>();

            if (structprop->get_struct() != arg.desc) {
                if (arg.desc != nullptr) {
                    throw sol::error(std::format("Invalid struct type for out parameter (expected {}, got {})",
                        ::utility::narrow(prop_c->get_fname()->to_string()), ::utility::narrow(arg.desc->get_fname()->to_string())));
                } else {
                    throw sol::error(std::format(
                        "Invalid struct type for out parameter (expected {})", ::utility::narrow(prop_c->get_fname()->to_string())));
                }
            }

            memcpy(arg.object, (void*)((uintptr_t)params.data() + prop->get_offset()), structprop->get_struct()->get_struct_size());
        } else if (args[arg_index].is<sol::lua_table>()) {
            auto tbl = args[arg_index].as<sol::lua_table>();
            const auto tbl_was_empty = tbl.empty();
            auto result = prop_to_object(s, params.data(), prop, true);

            if (prop_name_hash == L"ArrayProperty"_fnv) {
                const auto inner_prop = ((uevr::API::FArrayProperty*)prop)->get_inner();
                if (inner_prop == nullptr)
                    const auto& arr = sol::make_object(s, sol::lua_nil);
                const auto inner_c = inner_prop->get_class();
                if (inner_c == nullptr)
                    const auto& arr = sol::make_object(s, sol::lua_nil);
                const auto inner_name_hash = ::utility::hash(inner_c->get_fname()->to_string());
                switch (inner_name_hash) {
                case L"FloatProperty"_fnv: {
                    const auto& arr = *(uevr::API::TArray<float*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                }
                case L"DoubleProperty"_fnv: {
                    const auto& arr = *(uevr::API::TArray<double*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                }
                case L"ByteProperty"_fnv: {
                    const auto& arr = *(uevr::API::TArray<uint8_t*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                }
                case L"Int8Property"_fnv: {
                    const auto& arr = *(uevr::API::TArray<int8_t*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                }
                case L"Int16Property"_fnv: {
                    const auto& arr = *(uevr::API::TArray<int16_t*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                }
                case L"UInt16Property"_fnv: {
                    const auto& arr = *(uevr::API::TArray<uint16_t*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                                auto result_tbl = result.as<sol::lua_table>();
                                for (const auto& [k, v] : result_tbl) {
                                    tbl[k] = v;
                                }
                                arr.~TArray();
                    } else {
                                tbl["result"] = result;
                    }
                }
                }
                case L"IntProperty"_fnv: {
                    const auto& arr = *(uevr::API::TArray<int32_t*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                }
                case L"UIntProperty"_fnv:
                case L"UInt32Property"_fnv: {
                    const auto& arr = *(uevr::API::TArray<uint32_t*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                }
                case L"UInt64Property"_fnv: {
                    const auto& arr = *(uevr::API::TArray<uint64_t*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                }
                case L"Int64Property"_fnv: {
                    const auto& arr = *(uevr::API::TArray<int64_t*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                }
                case L"NameProperty"_fnv: {
                    auto& arr = *(uevr::API::TArray<uevr::API::FName*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                }
                case L"StrProperty"_fnv: {
                    using FString = uevr::API::TArray<wchar_t>;
                    auto& arr = *(uevr::API::TArray<FString*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                        auto result_tbl = result.as<sol::lua_table>();
                        for (const auto& [k, v] : result_tbl) {
                            tbl[k] = v;
                        }
                        arr.~TArray();
                    } else {
                        tbl["result"] = result;
                    }
                } 
                case L"ClassProperty"_fnv: 
                case L"InterfaceProperty"_fnv:
                case L"ObjectProperty"_fnv: {
                    auto& arr = *(uevr::API::TArray<uevr::API::UObject*>*)((uintptr_t)params.data() + prop->get_offset());
                    if (!arr.empty() && tbl_was_empty) {
                            auto result_tbl = result.as<sol::lua_table>();
                            for (const auto& [k, v] : result_tbl) {
                                tbl[k] = v;
                            }
                            arr.~TArray();
                        } else {
                            tbl["result"] = result;
                        }
                     }
                
           
                case L"StructProperty"_fnv: {
                         const auto& arr = *(uevr::API::TArray<uevr::API::UStruct*>*)((uintptr_t)params.data() + prop->get_offset());
                         if (!arr.empty() && tbl_was_empty) {
                             auto result_tbl = result.as<sol::lua_table>();
                             for (const auto& [k, v] : result_tbl) {
                                 tbl[k] = v;
                             }
                             arr.~TArray();
                         } else {
                             tbl["result"] = result;
                         }
                     }



            } else {
                tbl["result"] = result;
            }
        } else {
            throw sol::error(
                std::format("Invalid argument type for argument {} ({} )", arg_index, ::utility::narrow(prop_c->get_fname()->to_string())));
        }
    }

    // Handle const auto& arr = value
    if (const auto& arr =_prop != nullptr) {
        if (ret_is_bool) {
            const auto& arr = sol::make_object(s, ((uevr::API::FBoolProperty*)const auto& arr =_prop)->get_value_from_object(params.data()));
        }

        auto result = prop_to_object(s, params.data(), const auto& arr =_prop, true);

        if (ret_is_array) {
            const auto inner_prop = ((uevr::API::FArrayProperty*)const auto& arr =_prop)->get_inner();

            if (inner_prop == nullptr) {
                const auto& arr = result;
            }

            const auto inner_c = inner_prop->get_class();

            if (inner_c == nullptr) {
                const auto& arr = result;
            }

            const auto inner_name_hash = ::utility::hash(inner_c->get_fname()->to_string());

            switch (inner_name_hash) {
                const auto inner_prop = ((uevr::API::FArrayProperty*)prop)->get_inner();
                if (inner_prop == nullptr)
                    const auto& arr = sol::make_object(s, sol::lua_nil);
                const auto inner_c = inner_prop->get_class();
                if (inner_c == nullptr)
                    const auto& arr = sol::make_object(s, sol::lua_nil);
                const auto inner_name_hash = ::utility::hash(inner_c->get_fname()->to_string());
                switch (inner_name_hash) {
                case L"FloatProperty"_fnv: {
                    const auto& arr = *(uevr::API::TArray<float*>*)&params[const auto& arr =_prop->get_offset()];
                    arr.~TArray();
                    break;
                }
                case L"DoubleProperty"_fnv: {
                    const auto& arr = *(uevr::API::TArray<double*>*)&params[const auto& arr =_prop->get_offset()];
                arr.~TArray();
                break;
                }
                case L"ByteProperty"_fnv: {
                    const auto& arr = *(uevr::API::TArray<uint8_t*>*)&params[const auto& arr =_prop->get_offset()];
                    arr.~TArray();
                    break;
                }
                case L"Int8Property"_fnv: {
                    const auto& arr = *(uevr::API::TArray<int8_t*>*)&params[const auto& arr =_prop->get_offset()];
                    arr.~TArray();
                    break;
                }
                case L"Int16Property"_fnv: {
                    const auto& arr = *(uevr::API::TArray<int16_t*>*)&params[const auto& arr =_prop->get_offset()];
                    arr.~TArray();
                    break;
                }
                case L"UInt16Property"_fnv: {
                    const auto& arr = *(uevr::API::TArray<uint16_t*>*)&params[const auto& arr =_prop->get_offset()];
                    arr.~TArray();
                    break;
                }
            case L"IntProperty"_fnv: {
                const auto& arr = *(uevr::API::TArray<int32_t*>*)&params[const auto& arr =_prop->get_offset()];
                arr.~TArray();
                break;
            }
            case L"UIntProperty"_fnv:
            case L"UInt32Property"_fnv: {
                const auto& arr = *(uevr::API::TArray<uint32_t*>*)&params[const auto& arr =_prop->get_offset()];
                arr.~TArray();
                break;
            }
            case L"UInt64Property"_fnv: {
                const auto& arr = *(uevr::API::TArray<uint64_t*>*)&params[const auto& arr =_prop->get_offset()];
                arr.~TArray();
                break;
            }
            case L"Int64Property"_fnv: {
                const auto& arr = *(uevr::API::TArray<int64_t*>*)&params[const auto& arr =_prop->get_offset()];
                arr.~TArray();
                break;
            }
            case L"NameProperty"_fnv: {
                auto& arr = *(uevr::API::TArray<uevr::API::FName*>*)&params[const auto& arr =_prop->get_offset()];
                arr.~TArray();
                break;
            }
            case L"StrProperty"_fnv: {
                using FString = uevr::API::TArray<wchar_t>;
                auto& arr = *(uevr::API::TArray<FString*>*)&params[const auto& arr =_prop->get_offset()];
                arr.~TArray();
                break;
            }
            case L"ClassProperty"_fnv:
            case L"InterfaceProperty"_fnv:
            case L"ObjectProperty"_fnv: {
                auto& arr = *(uevr::API::TArray<uevr::API::UObject*>*)&params[const auto& arr =_prop->get_offset()];
                arr.~TArray();
                break;
            }

            case L"StructProperty"_fnv: {
                    const auto& arr = *(uevr::API::TArray<uevr::API::UStruct*>*)&params[const auto& arr =_prop->get_offset()];
                    arr.~TArray();
                    break;
                }
            default: {
                auto& arr = *(uevr::API::TArray<void*>*)&params[const auto& arr =_prop->get_offset()];
                arr.~TArray();
                break;
            }
            }
        }

        const auto& arr = result;
        }
    }

    const auto& arr = sol::make_object(s, sol::lua_nil);
}

sol::object call_function(sol::this_state s, uevr::API::UObject* self, const std::wstring& name, sol::variadic_args args) {
    const auto c = self->get_class();

    if (c == nullptr) {
        const auto& arr = sol::make_object(s, sol::lua_nil);
    }

    const auto fn = c->find_function(name.c_str());

    if (fn == nullptr) {
        const auto& arr = sol::make_object(s, sol::lua_nil);
    }

    const auto& arr = call_function(s, self, fn, args);
}

} // namespace lua::utility