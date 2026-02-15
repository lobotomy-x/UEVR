#include <format>
#include <string>
#include <cstdint>

#include <utility/String.hpp>

#include <datatypes/Vector.hpp>
#include <datatypes/Quaternion.hpp>
#include <datatypes/Matrix.hpp>
#include <datatypes/Transform.hpp>
#include <datatypes/StructObject.hpp>
#include <ScriptUtility.hpp>

namespace lua::utility {
    extern struct UE_ProxyPtr;
     
    using Vector2f = lua::datatypes::Vector2f;
    using Vector2d = lua::datatypes::Vector2d;
    using Vector3f = lua::datatypes::Vector3f;
    using Vector3d = lua::datatypes::Vector3d;
    using Vector4f = lua::datatypes::Vector4f;
    using Vector4d = lua::datatypes::Vector4d;
    using Quaternionf = lua::datatypes::Quaternionf;
    using Quaterniond = lua::datatypes::Quaterniond;
    using Matrix4f = lua::datatypes::Matrix4x4f;
    using Matrix4d = lua::datatypes::Matrix4x4d;

   using Transformf = lua::datatypes::Transform<Vector3f, Quaternionf>;
   using Transformd = lua::datatypes::Transform<Vector3d, Quaterniond>;
sol::object call_function(sol::this_state s, uevr::API::UObject* self, uevr::API::UFunction* fn, sol::variadic_args args);

uevr::API::UScriptStruct* get_vector_struct() {
    static auto vector_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector");
        const auto old_class = modern_class == nullptr ? uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Vector") : nullptr;

        return modern_class != nullptr ? modern_class : old_class;
    }();

    return vector_struct;
}

uevr::API::UScriptStruct* get_rotator_struct() {
    static auto rotator_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Rotator");
        const auto old_class = modern_class == nullptr ? uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Rotator") : nullptr;

        return modern_class != nullptr ? modern_class : old_class;
    }();

    return rotator_struct;
}

uevr::API::UScriptStruct* get_vector4_struct() {
    static auto vector_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector4");
        const auto old_class = modern_class == nullptr ? uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Vector4") : nullptr;
        return modern_class != nullptr ? modern_class : old_class;
    }();

    return vector_struct;
}

uevr::API::UScriptStruct* get_vector4d_struct() {
    static auto vector_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector4d");
        return modern_class;
    }();

    return vector_struct;
}

uevr::API::UScriptStruct* get_vector4f_struct() {
    static auto vector_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector4f");
        return modern_class;
    }();

    return vector_struct;
}

uevr::API::UScriptStruct* get_vector3d_struct() {
    static auto vector_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector3d");
        return modern_class;
    }();

    return vector_struct;
}

uevr::API::UScriptStruct* get_vector3f_struct() {
    static auto vector_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector3f");
        return modern_class;
    }();

    return vector_struct;
}

uevr::API::UScriptStruct* get_vector2d_struct() {
    static auto vector_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Vector2D");
        const auto old_class = modern_class == nullptr ? uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Vector2D") : nullptr;
        return modern_class != nullptr ? modern_class : old_class;
    }();

    return vector_struct;
}

uevr::API::UScriptStruct* get_linearcolor_struct() {
    static auto linear_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.LinearColor");
        if (modern_class != nullptr) return modern_class;
        return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.LinearColor");
    }();

    return linear_struct;
}

uevr::API::UScriptStruct* get_color_struct() {
    static auto color_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Color");
        if (modern_class != nullptr) return modern_class;
        return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Color");
    }();

    return color_struct;
}

uevr::API::UScriptStruct* get_quat_struct() {
    static auto quat_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Quat");
        if (modern_class != nullptr) return modern_class;
        return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Quat");
    }();

    return quat_struct;
}

uevr::API::UScriptStruct* get_transform_struct() {
    static auto transform_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Transform");
        const auto old_class = modern_class == nullptr ? uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/CoreUObject.Object.Transform") : nullptr;
        return modern_class != nullptr ? modern_class : old_class;
    }();

    return transform_struct;
}

uevr::API::UScriptStruct* get_hitresult_struct() {
    static auto hitresult_struct = []() {
        const auto modern_class = uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/Engine.HitResult");
        if (modern_class != nullptr) return modern_class;
        return uevr::API::get()->find_uobject<uevr::API::UScriptStruct>(L"ScriptStruct /Script/Engine.HitResult");
    }();

    return hitresult_struct;
}
template <typename T>
void SyncLuaTableToTArray(const sol::lua_table& lua_table, void* array_ptr, std::vector<std::vector<T>>& dynamic_storage) {
    // 1. Cast the raw buffer to the UEVR TArray structure
    auto& ue_arr = *(uevr::API::TArray<T>*)array_ptr;

    // 2. Create a stable backing store for this specific call (prevent GC issues)
    auto& backing_vec = dynamic_storage.emplace_back();
    backing_vec.reserve(lua_table.size());

    // 3. Populate from Lua (Note: Lua is 1-indexed)
    for (size_t i = 1; i <= lua_table.size(); ++i) {
        backing_vec.push_back(lua_table[i].get<T>());
    }

    // 4. Update the UE TArray headers to point to our new data
    ue_arr.data = backing_vec.data();
    ue_arr.count = (int32_t)backing_vec.size();
    ue_arr.capacity = (int32_t)backing_vec.size();
}
sol::table TArrayToLuaTable(sol::state_view& lua, void* array_ptr, const std::string& type_name) {
    sol::table out_table = lua.create_table();

    // We treat it as UObject* by default for UEVR
    auto& ue_arr = *(uevr::API::TArray<uevr::API::UObject*>*)array_ptr;

    for (int32_t i = 0; i < ue_arr.count; ++i) {
        // Lua index i+1
        out_table[i + 1] = ue_arr.data[i];
    }
    return out_table;
}
template <typename T> void ProcessTArray(const sol::object& arg, void* params_ptr) {
    if (arg.is<sol::lua_table>()) {
        sol::lua_table tbl = arg.as<sol::lua_table>();
        auto& ue_arr = *(uevr::API::TArray<T>*)params_ptr;

        // Use a type that isn't specialized like vector<bool>
        using StorageType = std::conditional_t<std::is_same_v<T, bool>, uint8_t, T>;

        static std::vector<StorageType> buffer;
        buffer.clear();
        buffer.reserve(tbl.size());

        for (size_t i = 1; i <= tbl.size(); ++i) {
            // Convert Lua value to T, then store in our compatible buffer
            buffer.push_back((StorageType)tbl[i].get<T>());
        }

        // Now .data() will work because it's a vector<uint8_t>
        ue_arr.data = (T*)buffer.data();
        ue_arr.count = (int)buffer.size();
        ue_arr.capacity = (int)buffer.size();
    }
}
using DispatchFunc = void (*)(const sol::object&, void*);
     using FString = uevr::API::TArray<wchar_t>;
static const std::unordered_map<std::string_view, DispatchFunc> TypeDispatcher = {


    {"FloatProperty", ProcessTArray<float>}, 
    {"ObjectProperty", ProcessTArray<uevr::API::UObject*>}, 
    {"BoolProperty", ProcessTArray<uint8_t>},
    {"ByteProperty", ProcessTArray<uint8_t>},
    {"DoubleProperty", ProcessTArray<double>},
    {"NameProperty", ProcessTArray<uevr::API::FName*>},
    {"StringProperty", ProcessTArray<FString*>}, 
    {"Int8Property", ProcessTArray<int8_t>},
    {"Int16Property", ProcessTArray<int16_t>},
    {"UIntProperty", ProcessTArray<uint16_t>},
    {"IntProperty", ProcessTArray<int32_t>},
    {"UIntProperty", ProcessTArray<uint32_t>},
    {"UInt32Property", ProcessTArray<uint32_t>},
    {"UInt64Property", ProcessTArray<uint64_t>},
    {"Int64Property", ProcessTArray<int64_t>},
    {"InterfaceProperty", ProcessTArray<uevr::API::UObject*>},
    {"ClassProperty", ProcessTArray<uevr::API::UClass*>},
    {"StructProperty", ProcessTArray<uevr::API::UStruct*>},
};

// General overload that header declares. Forward to existing implementations when possible.
sol::object prop_to_object(sol::this_state s, void* self, const int32_t offset, const size_t name_hash, uevr::API::FProperty* desc, bool is_self_temporary) {
    if (desc != nullptr) {
        return prop_to_object(s, self, desc, is_self_temporary);
    }

    // Fallback: cannot handle without a property descriptor, return nil
    return sol::make_object(s, sol::lua_nil);
}

bool is_ue5() {
    static auto cached_result = []() {
        const auto c = get_vector_struct();

        if (c == nullptr) {
            return false;
        }

        return c->get_struct_size() == sizeof(glm::dvec3);
    }();
    
    return cached_result;
}

sol::object prop_to_object(sol::this_state s, void* self, uevr::API::FProperty* desc, bool is_self_temporary) {
    const auto propc = desc->get_class();

    if (propc == nullptr) {
        return sol::make_object(s, sol::lua_nil);
    }

    const auto name_hash = ::utility::hash(propc->get_fname()->to_string());
    const auto offset = desc->get_offset();

    switch (name_hash) {
    case L"BoolProperty"_fnv:
    {
        const auto fbp = (uevr::API::FBoolProperty*)desc;
        return sol::make_object(s, fbp->get_value_from_object(self));
    }
    case L"FloatProperty"_fnv:
        return sol::make_object(s, *(float*)((uintptr_t)self + offset));
    case L"DoubleProperty"_fnv:
        return sol::make_object(s, *(double*)((uintptr_t)self + offset));
    case L"ByteProperty"_fnv:
        return sol::make_object(s, *(uint8_t*)((uintptr_t)self + offset));
    case L"Int8Property"_fnv:
        return sol::make_object(s, *(int8_t*)((uintptr_t)self + offset));
    case L"Int16Property"_fnv:
        return sol::make_object(s, *(int16_t*)((uintptr_t)self + offset));
    case L"UInt16Property"_fnv:
        return sol::make_object(s, *(uint16_t*)((uintptr_t)self + offset));
    case L"IntProperty"_fnv:
        return sol::make_object(s, *(int32_t*)((uintptr_t)self + offset));
    case L"UIntProperty"_fnv:
    case L"UInt32Property"_fnv:
        return sol::make_object(s, *(uint32_t*)((uintptr_t)self + offset));
    case L"UInt64Property"_fnv:
        return sol::make_object(s, *(uint64_t*)((uintptr_t)self + offset));
    case L"Int64Property"_fnv:
        return sol::make_object(s, *(int64_t*)((uintptr_t)self + offset));
    case L"EnumProperty"_fnv:
    {
        const auto ep = (uevr::API::FEnumProperty*)desc;
        const auto np = ep->get_underlying_prop();

        if (np == nullptr) {
            return sol::make_object(s, sol::lua_nil);
        }

        const auto np_c = np->get_class();

        if (np_c == nullptr) {
            return sol::make_object(s, sol::lua_nil);
        }

        const auto np_name_hash = ::utility::hash(np_c->get_fname()->to_string());

        switch (np_name_hash) {
        case L"FloatProperty"_fnv:
            return sol::make_object(s, *(float*)((uintptr_t)self + offset));
        case L"DoubleProperty"_fnv:
            return sol::make_object(s, *(double*)((uintptr_t)self + offset));
        case L"ByteProperty"_fnv:
            return sol::make_object(s, *(uint8_t*)((uintptr_t)self + offset));
        case L"Int8Property"_fnv:
            return sol::make_object(s, *(int8_t*)((uintptr_t)self + offset));
        case L"Int16Property"_fnv:
            return sol::make_object(s, *(int16_t*)((uintptr_t)self + offset));
        case L"UInt16Property"_fnv:
            return sol::make_object(s, *(uint16_t*)((uintptr_t)self + offset));
        case L"IntProperty"_fnv:
            return sol::make_object(s, *(int32_t*)((uintptr_t)self + offset));
        case L"UIntProperty"_fnv:
        case L"UInt32Property"_fnv:
            return sol::make_object(s, *(uint32_t*)((uintptr_t)self + offset));
        case L"UInt64Property"_fnv:
            return sol::make_object(s, *(uint64_t*)((uintptr_t)self + offset));
        case L"Int64Property"_fnv:
            return sol::make_object(s, *(int64_t*)((uintptr_t)self + offset));
        };

        return sol::make_object(s, sol::lua_nil);
    }
    case L"NameProperty"_fnv:
        return sol::make_object(s, *(uevr::API::FName*)((uintptr_t)self + offset));
    case L"StrProperty"_fnv:
    {
        using FString = uevr::API::TArray<wchar_t>;
        const auto& str = *(FString*)((uintptr_t)self + offset);
        if (str.data == nullptr || str.count == 0) {
            return sol::make_object(s, "");
        }
        return sol::make_object(s, std::wstring(str.data, str.count));
    }
    case L"InterfaceProperty"_fnv:
    case L"ObjectProperty"_fnv:
        if (*(uevr::API::UObject**)((uintptr_t)self + offset) == nullptr) {
            return sol::make_object(s, sol::lua_nil);
        }

        return sol::make_object(s, *(uevr::API::UObject**)((uintptr_t)self + offset));
    case L"ClassProperty"_fnv:
        if (*(uevr::API::UClass**)((uintptr_t)self + offset) == nullptr) {
            return sol::make_object(s, sol::lua_nil);
        }

        return sol::make_object(s, *(uevr::API::UClass**)((uintptr_t)self + offset));
    case L"Function"_fnv:
        return sol::make_object(s, (uevr::API::UFunction*)desc); // Not actually a property inside the object
    case L"StructProperty"_fnv:
    {
        const auto struct_data = (void*)((uintptr_t)self + offset);
        const auto struct_desc = ((uevr::API::FStructProperty*)desc)->get_struct();

        if (struct_desc == nullptr) {
            return sol::make_object(s, sol::lua_nil);
        }
        if (struct_desc == get_linearcolor_struct()) {
            if (is_self_temporary)
                return sol::make_object(s, *(lua::datatypes::Vector4f*)struct_data);
            return sol::make_object(s, (lua::datatypes::Vector4f*)struct_data);
        }

        if (struct_desc == get_vector4_struct() || struct_desc == get_vector4d_struct()) {
            if (is_ue5()) {
            if (is_self_temporary)
                return sol::make_object(s, *(lua::datatypes::Vector4d*)struct_data);
            return sol::make_object(s, (lua::datatypes::Vector4d*)struct_data); 
            }
            else{
            if (is_self_temporary)
                return sol::make_object(s, *(lua::datatypes::Vector4f*)struct_data);
            return sol::make_object(s, (lua::datatypes::Vector4f*)struct_data);
        }

        }

    if (struct_desc == get_vector2d_struct()) {
            if (is_ue5()) {
            if (is_self_temporary)
                return sol::make_object(s, *(lua::datatypes::Vector2d*)struct_data);
            return sol::make_object(s, (lua::datatypes::Vector2d*)struct_data);
        }
        else {
                if (is_self_temporary)
                    return sol::make_object(s, *(lua::datatypes::Vector2f*)struct_data);
                return sol::make_object(s, (lua::datatypes::Vector2f*)struct_data);
    }
}


        if (struct_desc == get_quat_struct()) {
    if (is_ue5()) {
            if (is_self_temporary)
                return sol::make_object(s, *(lua::datatypes::Quaterniond*)struct_data);
            return sol::make_object(s, (lua::datatypes::Quaterniond*)struct_data);
        }
else {
        if (is_self_temporary)
            return sol::make_object(s, *(lua::datatypes::Quaternionf*)struct_data);
        return sol::make_object(s, (lua::datatypes::Quaternionf*)struct_data);
        }
}


        if (struct_desc == get_vector_struct() || struct_desc == get_rotator_struct()) {
            if (is_ue5()) {
                if (is_self_temporary) {
                    return sol::make_object(s, *(lua::datatypes::Vector3d*)struct_data);
                } else {
                    return sol::make_object(s, (lua::datatypes::Vector3d*)struct_data);
                }
            }

            if (is_self_temporary) {
                return sol::make_object(s, *(lua::datatypes::Vector3f*)struct_data);
            } else {
                return sol::make_object(s, (lua::datatypes::Vector3f*)struct_data);
            }
        }

        if (is_self_temporary) {
            auto new_object = std::make_unique<lua::datatypes::StructObject>(struct_desc);
            memcpy(new_object->object, struct_data, new_object->created_object.size());
            return sol::make_object(s, std::move(new_object));
        }

        auto new_object = std::make_unique<lua::datatypes::StructObject>(struct_data, struct_desc);
        return sol::make_object(s, std::move(new_object));
    }
    case L"ArrayProperty"_fnv:
    {
        const auto inner_prop = ((uevr::API::FArrayProperty*)desc)->get_inner();

        if (inner_prop == nullptr) {
            return sol::make_object(s, sol::lua_nil);
        }

        const auto inner_c = inner_prop->get_class();

        if (inner_c == nullptr) {
            return sol::make_object(s, sol::lua_nil);
        }

        const auto inner_name_hash = ::utility::hash(inner_c->get_fname()->to_string());

        switch (inner_name_hash) {
        case L"InterfaceProperty"_fnv:
        case L"ObjectProperty"_fnv:
        {
            const auto& arr = *(uevr::API::TArray<uevr::API::UObject*>*)((uintptr_t)self + offset);

            if (arr.data == nullptr || arr.count == 0) {
                return sol::make_object(s, sol::lua_nil);
            }

            auto lua_arr = sol::state_view{s}.create_table();

            for (size_t i = 0; i < arr.count; ++i) {
                lua_arr[i + 1] = sol::make_object(s, arr.data[i]);
            }

            return sol::make_object(s, lua_arr);
        }
        case L"ClassProperty"_fnv:
        {
            const auto& arr = *(uevr::API::TArray<uevr::API::UClass*>*)((uintptr_t)self + offset);

            if (arr.data == nullptr || arr.count == 0) {
                return sol::make_object(s, sol::lua_nil);
            }

            auto lua_arr = sol::state_view{s}.create_table();

            for (size_t i = 0; i < arr.count; ++i) {
                lua_arr[i + 1] = sol::make_object(s, arr.data[i]);
            }

            return sol::make_object(s, lua_arr);
        }
        case L"NameProperty"_fnv: {
            const auto& arr = *(uevr::API::TArray<uevr::API::FName*>*)((uintptr_t)self + offset);

            if (arr.data == nullptr || arr.count == 0) {
                return sol::make_object(s, sol::lua_nil);
            }

            auto lua_arr = sol::state_view{s}.create_table();

            for (size_t i = 0; i < arr.count; ++i) {
                lua_arr[i + 1] = sol::make_object(s, arr.data[i]);
            }

            return sol::make_object(s, lua_arr);
        }

        // TODO: Add support for other types
        };

        return sol::make_object(s, sol::lua_nil);
    }
    };

    return sol::make_object(s, sol::lua_nil);
}

sol::object prop_to_object(sol::this_state s, void* self, uevr::API::UStruct* c, const std::wstring& name) {
    const auto desc = c->find_property(name.c_str());

    if (desc == nullptr) {
        if (auto fn = c->find_function(name.c_str()); fn != nullptr) {
            /*return sol::make_object(s, [self, s, fn](sol::variadic_args args) {
                return call_function(s, self, fn, args);
            });*/
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

    switch (name_hash) {
    case L"BoolProperty"_fnv:
    {
        const auto fbp = (uevr::API::FBoolProperty*)desc;
        fbp->set_value_in_object(self, value.as<bool>());
        return;
    }
    case L"FloatProperty"_fnv:
        *(float*)((uintptr_t)self + offset) = value.as<float>();
        return;
    case L"DoubleProperty"_fnv:
        *(double*)((uintptr_t)self + offset) = value.as<double>();
        return;
    case L"ByteProperty"_fnv:
        *(uint8_t*)((uintptr_t)self + offset) = value.as<uint8_t>();
        return;
    case L"Int8Property"_fnv:
        *(int8_t*)((uintptr_t)self + offset) = value.as<int8_t>();
        return;
    case L"Int16Property"_fnv:
        *(int16_t*)((uintptr_t)self + offset) = value.as<int16_t>();
        return;
    case L"UInt16Property"_fnv:
        *(uint16_t*)((uintptr_t)self + offset) = value.as<uint16_t>();
        return;
    case L"IntProperty"_fnv:
        *(int32_t*)((uintptr_t)self + offset) = value.as<int32_t>();
        return;
    case L"UIntProperty"_fnv:
    case L"UInt32Property"_fnv:
        *(uint32_t*)((uintptr_t)self + offset) = value.as<uint32_t>();
        return;
    case L"UInt64Property"_fnv:
        *(uint64_t*)((uintptr_t)self + offset) = value.as<uint64_t>();
        return;
    case L"Int64Property"_fnv:
        *(int64_t*)((uintptr_t)self + offset) = value.as<int64_t>();
        return;
    case L"EnumProperty"_fnv:
    {
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
            *(float*)((uintptr_t)self + offset) = value.as<float>();
            return;
        case L"DoubleProperty"_fnv:
            *(double*)((uintptr_t)self + offset) = value.as<double>();
            return;
        case L"ByteProperty"_fnv:
            *(uint8_t*)((uintptr_t)self + offset) = value.as<uint8_t>();
            return;
        case L"Int8Property"_fnv:
            *(int8_t*)((uintptr_t)self + offset) = value.as<int8_t>();
            return;
        case L"Int16Property"_fnv:
            *(int16_t*)((uintptr_t)self + offset) = value.as<int16_t>();
            return;
        case L"UInt16Property"_fnv:
            *(uint16_t*)((uintptr_t)self + offset) = value.as<uint16_t>();
            return;
        case L"IntProperty"_fnv:
            *(int32_t*)((uintptr_t)self + offset) = value.as<int32_t>();
            return;
        case L"UIntProperty"_fnv:
        case L"UInt32Property"_fnv:
            *(uint32_t*)((uintptr_t)self + offset) = value.as<uint32_t>();
            return;
        case L"UInt64Property"_fnv:
            *(uint64_t*)((uintptr_t)self + offset) = value.as<uint64_t>();
            return;
        case L"Int64Property"_fnv:
            *(int64_t*)((uintptr_t)self + offset) = value.as<int64_t>();
            return;
        };

        throw sol::error("Could not set enum property");
    }
    case L"NameProperty"_fnv:
        if (value.is<std::string>()) {
            const auto arg = ::utility::widen(value.as<std::string>());
            *(uevr::API::FName*)((uintptr_t)self + offset) = uevr::API::FName{arg};
        } else if (value.is<std::wstring>()) {
            const auto arg = value.as<std::wstring>();
            *(uevr::API::FName*)((uintptr_t)self + offset) = uevr::API::FName{arg};
        } else if (value.is<uevr::API::FName>()) {
            const auto arg = value.as<uevr::API::FName>();
            *(uevr::API::FName*)((uintptr_t)self + offset) = arg;
        } else {
            throw sol::error("Invalid argument type for FName");
        }

        return;
    case L"InterfaceProperty"_fnv:
    case L"ObjectProperty"_fnv:
        *(uevr::API::UObject**)((uintptr_t)self + offset) = value.as<uevr::API::UObject*>();
        return;
    case L"ClassProperty"_fnv:
        *(uevr::API::UClass**)((uintptr_t)self + offset) = value.as<uevr::API::UClass*>();
        return;
    case L"ArrayProperty"_fnv: 

    //{
    //        if (!value.is<sol::lua_table>()) {
    //            throw sol::error("Setting TArray from non-table is not implemented");
    //        }

    //        const auto inner_prop = ((uevr::API::FArrayProperty*)desc)->get_inner();
    //        if (inner_prop == nullptr)
    //            throw sol::error("Array property has no inner property");
    //        const auto inner_c = inner_prop->get_class();
    //        if (inner_c == nullptr)
    //            throw sol::error("Array inner property has no class");
    //        const auto inner_name_hash = ::utility::hash(inner_c->get_fname()->to_string());

    //        auto tbl = value.as<sol::table>();

    //        switch (inner_name_hash) {
    //        case L"FloatProperty"_fnv:
    //            return create_tarray_from_table<float>(s, self, offset, tbl);
    //        case L"DoubleProperty"_fnv:
    //            return create_tarray_from_table<double>(s, self, offset, tbl);
    //        case L"ByteProperty"_fnv:
    //            return create_tarray_from_table<uint8_t>(s, self, offset, tbl);
    //        case L"Int8Property"_fnv:
    //            return create_tarray_from_table<int8_t>(s, self, offset, tbl);
    //        case L"Int16Property"_fnv:
    //            return create_tarray_from_table<int16_t>(s, self, offset, tbl);
    //        case L"UInt16Property"_fnv:
    //            return create_tarray_from_table<uint16_t>(s, self, offset, tbl);
    //        case L"IntProperty"_fnv:
    //            return create_tarray_from_table<int32_t>(s, self, offset, tbl);
    //        case L"UIntProperty"_fnv:
    //        case L"UInt32Property"_fnv:
    //            return create_tarray_from_table<uint32_t>(s, self, offset, tbl);
    //        case L"UInt64Property"_fnv:
    //            return create_tarray_from_table<uint64_t>(s, self, offset, tbl);
    //        case L"Int64Property"_fnv:
    //            return create_tarray_from_table<int64_t>(s, self, offset, tbl);
    //        case L"InterfaceProperty"_fnv:
    //        case L"ClassProperty"_fnv:
    //        case L"ObjectProperty"_fnv:
    //            return create_tarray_from_table<uevr::API::UObject*>(s, self, offset, tbl);
            default:
                throw sol::error("Setting TArray for this element type is not implemented");
            //}
    
    case L"StrProperty"_fnv:
    {
        const auto arg_obj = value;
        using FString = uevr::API::TArray<wchar_t>;

        auto& fstr = *(FString*)&*(uevr::API::TArray<wchar_t>*)((uintptr_t)self + offset);

        if (arg_obj.is<std::wstring>()) {
            const auto src = arg_obj.as<std::wstring>();

            // Dynamic strings is used for temporary stuff like function calls
            // when we set a property it can exist permanently
            if (dynamic_strings != nullptr) {
                auto buffer = std::make_unique<wchar_t[]>(src.size() + 1);
                std::copy(src.begin(), src.end(), buffer.get());
                buffer[src.size()] = L'\0';
    
                fstr.count = src.size() + 1;
                fstr.capacity = fstr.count;
                fstr.data = buffer.get();

                dynamic_strings->push_back(std::move(buffer));
            } else {
                // Use FMalloc (TODO)

            }
        } else if (arg_obj.is<std::string>()) {
            const auto src = ::utility::widen(arg_obj.as<std::string>());

            if (dynamic_strings != nullptr) {
                auto buffer = std::make_unique<wchar_t[]>(src.size() + 1);
                std::copy(src.begin(), src.end(), buffer.get());
                buffer[src.size()] = L'\0';

                fstr.count = src.size() + 1;
                fstr.capacity = fstr.count;
                fstr.data = buffer.get();

                dynamic_strings->push_back(std::move(buffer));
            } else {
                // Use FMalloc (TODO)
            }
        } else if (arg_obj.is<wchar_t*>()) {
            const auto src = std::wstring_view{arg_obj.as<wchar_t*>()};

            if (dynamic_strings != nullptr) {
                auto buffer = std::make_unique<wchar_t[]>(src.size() + 1);
                std::copy(src.begin(), src.end(), buffer.get());
                buffer[src.size()] = L'\0';

                fstr.count = src.size() + 1;
                fstr.capacity = fstr.count;
                fstr.data = buffer.get();

                dynamic_strings->push_back(std::move(buffer));
            } else {
                // Use FMalloc (TODO)
            }
        } else {
            throw sol::error("Invalid argument type for FString");
        }

        return;
    }
    case L"StructProperty"_fnv:
    {
        const auto struct_desc = ((uevr::API::FStructProperty*)desc)->get_struct();

        if (struct_desc == nullptr) {
            throw sol::error("Struct property has no struct");
        }

        if (value.is<sol::lua_table>()) {
            // Go through each key in the table and set each property in the struct to the value (if it exists)
            for (const auto& [key, val] : value.as<sol::table>()) {
                if (!key.is<std::wstring>()) {
                    throw sol::error("Invalid key type for struct property (expected string)");
                }
                
                const auto prop = struct_desc->find_property(key.as<std::wstring>().c_str());

                if (prop == nullptr) {
                    throw sol::error(std::format("Struct property '{}' not found in {}", ::utility::narrow(key.as<std::wstring>()), ::utility::narrow(struct_desc->get_fname()->to_string())));
                }

                set_property(s, (void*)((uintptr_t)self + offset), struct_desc, prop, val, dynamic_strings);
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

            memcpy((void*)((uintptr_t)self + offset), arg.object, struct_desc->get_struct_size());
        }
 else if (struct_desc == get_vector_struct() || struct_desc == get_rotator_struct()) { // Same layout
            if (value.is<lua::datatypes::Vector3f>()) {
                const auto arg = value.as<lua::datatypes::Vector3f>();

                if (is_ue5()) {
                    *(lua::datatypes::Vector3d*)((uintptr_t)self + offset) = arg;
                } else {
                    *(lua::datatypes::Vector3f*)((uintptr_t)self + offset) = arg;
                }
            } else if (value.is<lua::datatypes::Vector3d>()) {
                const auto arg = value.as<lua::datatypes::Vector3d>();

                if (is_ue5()) {
                    *(lua::datatypes::Vector3d*)((uintptr_t)self + offset) = arg;
                } else {
                    *(lua::datatypes::Vector3f*)((uintptr_t)self + offset) = arg;
                }
            } else {
                throw sol::error("Invalid argument type for FVector");
            }
        
}

     else if (struct_desc == get_vector2d_struct() ) { // Same layout
            if (value.is<lua::datatypes::Vector2d>()) {
                const auto arg = value.as<lua::datatypes::Vector2f>();

                if (is_ue5()) {
                    *(lua::datatypes::Vector2d*)((uintptr_t)self + offset) = arg;
                } else {
                    *(lua::datatypes::Vector2f*)((uintptr_t)self + offset) = arg;
                }
            } else if (value.is<lua::datatypes::Vector2d>()) {
                const auto arg = value.as<lua::datatypes::Vector2d>();

                if (is_ue5()) {
                    *(lua::datatypes::Vector2d*)((uintptr_t)self + offset) = arg;
                } else {
                    *(lua::datatypes::Vector2f*)((uintptr_t)self + offset) = arg;
                }
            }
}

      else if (struct_desc == get_vector4d_struct()) { // Same layout
    if (value.is<lua::datatypes::Vector4d>()) {
        const auto arg = value.as<lua::datatypes::Vector4f>();

        if (is_ue5()) {
            *(lua::datatypes::Vector4d*)((uintptr_t)self + offset) = arg;
        } else {
            *(lua::datatypes::Vector4f*)((uintptr_t)self + offset) = arg;
        }
    } else if (value.is<lua::datatypes::Vector4d>()) {
        const auto arg = value.as<lua::datatypes::Vector4d>();

        if (is_ue5()) {
            *(lua::datatypes::Vector4d*)((uintptr_t)self + offset) = arg;
        } else {
            *(lua::datatypes::Vector4f*)((uintptr_t)self + offset) = arg;
        }
    }
} 




 else if (struct_desc == get_quat_struct() ) { 
            if (value.is<lua::datatypes::Quaternionf>()) {
                const auto arg = value.as<lua::datatypes::Quaternionf>();

                if (is_ue5()) {
                    *(lua::datatypes::Quaterniond*)((uintptr_t)self + offset) = arg;
                } else {
                    *(lua::datatypes::Quaterniond*)((uintptr_t)self + offset) = arg;
                }
            } else if (value.is<lua::datatypes::Quaterniond>()) {
                const auto arg = value.as<lua::datatypes::Quaterniond>();

                if (is_ue5()) {
                    *(lua::datatypes::Quaterniond*)((uintptr_t)self + offset) = arg;
                } else {
                    *(lua::datatypes::Quaternionf*)((uintptr_t)self + offset) = arg;
                }
            } else {
                throw sol::error("Invalid argument type for quat");
            }
        }


 else {
            throw sol::error("Invalid argument type for struct property");
        }

        return;
    }
    };

    // NONE
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
        return sol::make_object(s, sol::lua_nil);
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

    uevr::API::FProperty* return_prop{nullptr};
    bool ret_is_bool{false};
    bool ret_is_array{false};

    //std::vector<uint8_t> dynamic_data{};
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

        if (prop_desc->is_return_param()) {
            return_prop = prop_desc;

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
        } else if (arg_hash == L"ArrayProperty"_fnv) {
            const auto inner_prop = ((uevr::API::FArrayProperty*)prop_desc)->get_inner();

            if (inner_prop == nullptr) {
                continue;
            }

            const auto inner_c = inner_prop->get_class();

            if (inner_c == nullptr) {
                continue;
            }

            const auto inner_name_hash = ::utility::hash(inner_c->get_fname()->to_string());

            switch (inner_name_hash) {
            case L"InterfaceProperty"_fnv:
            case L"ObjectProperty"_fnv:
            case L"ClassProperty"_fnv:
            {
                const auto arg_obj = args[args_index++];
                
                if (arg_obj.is<sol::lua_table>()) {
                    const auto arg_table = arg_obj.as<sol::lua_table>();

                    auto& arr = *(uevr::API::TArray<uevr::API::UObject*>*)&params[offset];

                    //if (!prop_desc->is_out_param()) {
                        auto& dynamic_arr = dynamic_object_arrays.emplace_back();
                        dynamic_arr.resize(arg_table.size());

                        for (size_t i = 0; i < arg_table.size(); ++i) {
                            dynamic_arr[i] = arg_table[i + 1];
                        }

                        arr.count = dynamic_arr.size();
                        arr.capacity = dynamic_arr.size();
                        arr.data = dynamic_arr.data();
                    //} else {
                       //throw sol::error("Cannot set an out parameter with an array (yet)");
                    //}
                } else {
                    throw sol::error("Invalid argument type for ArrayProperty<ObjectProperty>");
                }
            }
            default:
                continue;
            }
        } else {
            // Might need to check if this causes issues
            if (prop_desc->is_out_param() && args[args_index].is<sol::lua_table>()) {
                args_index++;
                continue; // This means the caller wants to actually use this as an out parameter and not set it
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
                    throw sol::error(std::format("Invalid struct type for out parameter (expected {}, got {})", ::utility::narrow(prop_c->get_fname()->to_string()), ::utility::narrow(arg.desc->get_fname()->to_string())));
                } else {
                    throw sol::error(std::format("Invalid struct type for out parameter (expected {})", ::utility::narrow(prop_c->get_fname()->to_string())));
                }
            }

            memcpy(arg.object, (void*)((uintptr_t)params.data() + prop->get_offset()), structprop->get_struct()->get_struct_size());
        } else if (args[arg_index].is<sol::lua_table>()) {
            auto tbl = args[arg_index].as<sol::lua_table>();
            const auto tbl_was_empty = tbl.empty();
            auto result = prop_to_object(s, params.data(), prop, true);

            if (prop_name_hash == L"ArrayProperty"_fnv) {
                auto& arr = *(uevr::API::TArray<uevr::API::UObject*>*)((uintptr_t)params.data() + prop->get_offset());

                if (!arr.empty() && tbl_was_empty) {
                    // Shallow copy the array into the table
                    auto result_tbl = result.as<sol::lua_table>();
                    for (const auto& [k, v] : result_tbl) {
                        tbl[k] = v;
                    }
                    
                    arr.~TArray(); // This should be safe, as this is not our array
                } else {
                    tbl["result"] = result;
                }
            } else {
                tbl["result"] = result;
            }
        } else if (args[args_index].is<sol::nil_t>()) {
            // Do nothing
        } else {
            throw sol::error(std::format("Invalid argument type for argument {} ({})", arg_index, ::utility::narrow(prop_c->get_fname()->to_string())));
        }
    }

    // Handle return value
    if (return_prop != nullptr) {
        if (ret_is_bool) {
            return sol::make_object(s, ((uevr::API::FBoolProperty*)return_prop)->get_value_from_object(params.data()));
        }

        auto result = prop_to_object(s, params.data(), return_prop, true);

        if (ret_is_array) {
            const auto inner_prop = ((uevr::API::FArrayProperty*)return_prop)->get_inner();

            if (inner_prop == nullptr) {
                return result;
            }

            const auto inner_c = inner_prop->get_class();

            if (inner_c == nullptr) {
                return result;
            }

            const auto inner_name_hash = ::utility::hash(inner_c->get_fname()->to_string());

            switch (inner_name_hash) {
            case L"InterfaceProperty"_fnv:
            case L"ClassProperty"_fnv:
            case L"ObjectProperty"_fnv:
            {
                //printf("ArrayProperty<ObjectProperty> cleanup\n");
                auto& arr = *(uevr::API::TArray<uevr::API::UObject*>*)&params[return_prop->get_offset()];
                arr.~TArray();
                break;
            }
            default:
            {
                //printf("ArrayProperty cleanup\n");
                // This will not work correctly on non-trivial types, but... we'll deal with that later
                auto& arr = *(uevr::API::TArray<void*>*)&params[return_prop->get_offset()];
                arr.~TArray();
                break;
            }
            }
        }

        return result;
    }

    return sol::make_object(s, sol::lua_nil);
}

sol::object call_function(sol::this_state s, uevr::API::UObject* self, const std::wstring& name, sol::variadic_args args) {
    const auto c = self->get_class();

    if (c == nullptr) {
        return sol::make_object(s, sol::lua_nil);
    }

    const auto fn = c->find_function(name.c_str());

    if (fn == nullptr) {
        return sol::make_object(s, sol::lua_nil);
    }

    return call_function(s, self, fn, args);
}
}