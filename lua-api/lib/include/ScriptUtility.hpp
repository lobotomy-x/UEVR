#pragma once

#include "ScriptPrerequisites.hpp"
#include <uevr/API.hpp>

namespace lua::utility {
uevr::API::UScriptStruct* get_vector_struct();
uevr::API::UScriptStruct* get_vector4_struct();
uevr::API::UScriptStruct* get_vector4d_struct();
uevr::API::UScriptStruct* get_vector4f_struct();
uevr::API::UScriptStruct* get_vector3d_struct();
uevr::API::UScriptStruct* get_vector3f_struct();
uevr::API::UScriptStruct* get_vector2d_struct();
uevr::API::UScriptStruct* get_quat_struct();
uevr::API::UScriptStruct* get_transform_struct();
uevr::API::UScriptStruct* get_transform3f_struct();
uevr::API::UScriptStruct* get_transform3d_struct();

uevr::API::UScriptStruct* get_linearcolor_struct();
uevr::API::UScriptStruct* get_color_struct();
    bool is_ue5();
   sol::object prop_to_object(sol::this_state s, void* self, const int32_t offset, const size_t name_hash, uevr::API::FProperty* desc = nullptr, bool is_self_temporary = false);
    sol::object prop_to_object(sol::this_state s, void* self, uevr::API::FProperty* desc, bool is_self_temporary = false);
    sol::object prop_to_object(sol::this_state s, void* self, uevr::API::UStruct* desc, const std::wstring& name);
    sol::object prop_to_object(sol::this_state s, uevr::API::UObject* self, const std::wstring& name);

    void set_property(sol::this_state s, void* self, uevr::API::UStruct* c, uevr::API::FProperty* desc, sol::object value, std::vector<std::unique_ptr<wchar_t[]>>* dynamic_strings = nullptr);
    void set_property(sol::this_state s, void* self, uevr::API::UStruct* c, const std::wstring& name, sol::object value, std::vector<std::unique_ptr<wchar_t[]>>* dynamic_strings = nullptr);
    void set_property(sol::this_state s, uevr::API::UObject* self, const std::wstring& name, sol::object value, std::vector<std::unique_ptr<wchar_t[]>>* dynamic_strings = nullptr);

    sol::object call_function(sol::this_state s, uevr::API::UObject* self, uevr::API::UFunction* fn, sol::variadic_args args);
    sol::object call_function(sol::this_state s, uevr::API::UObject* self, const std::wstring& name, sol::variadic_args args);

    template<typename T>
    inline T read_t_struct(void* self, uevr::API::UStruct* c, size_t offset) {
        size_t size = 0;

        if (c->is_a(uevr::API::UScriptStruct::static_class())) {
            auto script_struct = reinterpret_cast<uevr::API::UScriptStruct*>(c);

            size = script_struct->get_struct_size();
        } else {
            size = c->get_properties_size();
        }

        if (offset + sizeof(T) > size) {
            throw sol::error("Offset out of bounds");
        }

        return *(T*)((uintptr_t)self + offset);
    }

    template <typename T>
    inline void write_t_struct(void* self, uevr::API::UStruct* c, size_t offset, T value) {
        size_t size = 0;
        if (c->is_a(uevr::API::UScriptStruct::static_class())) {
            auto script_struct = reinterpret_cast<uevr::API::UScriptStruct*>(c);

            size = script_struct->get_struct_size();
        } else {
            size = c->get_properties_size();
        }

        if (offset + sizeof(T) > size) {
            throw sol::error("Offset out of bounds");
        }

        *(T*)((uintptr_t)self + offset) = value;
    }

    template <typename T>
    void write_t(uevr::API::UObject* self, size_t offset, T value) {
        write_t_struct<T>(self, self->get_class(), offset, value);
    }

    template<typename T>
    T read_t(uevr::API::UObject* self, size_t offset) {
        return read_t_struct<T>(self, self->get_class(), offset);
    }                                      

    // Convert a UEVR TArray<T> to a Lua table. Returns lua_nil if array is empty or null.
    template <typename Elem> 
inline sol::object tarray_to_table(sol::this_state s, const uevr::API::TArray<Elem>& arr) {
        if (arr.data == nullptr || arr.count == 0) {
            return sol::make_object(s, sol::lua_nil);
        }

        auto lua_arr = sol::state_view{s}.create_table();
        for (int32_t i = 0; i < arr.count; ++i) {
            lua_arr[i + 1] = sol::make_object(s, arr.data[i]);
        }

        return sol::make_object(s, lua_arr);
    }
    template<typename T>
    inline sol::object tarray_to_table_ex(
            sol::this_state s, const uevr::API::TArray<T>& arr, const int32_t offset, const size_t name_hash) {
        if (arr.data == nullptr) {
            return sol::make_object(s, sol::lua_nil);
        }
        if (arr.count == 0) {
            return sol::state_view{s}.create_table();
        }
        auto lua_arr = sol::state_view{s}.create_table();
        for (int32_t i = 0; i < arr.count; ++i) {
            lua_arr[i + 1] = prop_to_object(s, arr.data[i], offset, name_hash, nullptr, true);
        }

        return sol::make_object(s, lua_arr);
    }

      template <typename T>
    inline void create_tarray_from_table(sol::this_state s, uintptr_t address, sol::table tbl) {
    
        using TARRAY = uevr::API::TArray<T>;

        auto& tarr = *(TARRAY*)&*(uevr::API::TArray<T>*)(address);

        for (int32_t i = 0; i < tbl.size(); ++i) {
            tarr[i] = sol::object(tbl[i+1]).as<T>();
        }

    }


}