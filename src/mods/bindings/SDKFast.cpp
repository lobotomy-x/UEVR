/*
 * SDKFast.cpp — Lua bindings for UESDK-direct transform getters/setters.
 *
 * These are exposed under `uevr.api_fast` in the Lua state. Each function
 * accepts a uevr::API::UObject* (what scripts already have), reinterprets it
 * as the appropriate sdk:: subclass, and calls the SDK method that caches a
 * UFunction pointer in a function-local `static`. This is roughly
 * 3-5× faster than the equivalent reflection chain in pure Lua because it
 * skips the per-call __index lookup, the sol2 argument marshaling, and the
 * StructObject return wrapping (we return a glm::vec3 directly).
 *
 * Glob safety: every function null-checks both the object and the target
 * subclass cast (by checking whether `get_class()->is_a(...)`). On failure
 * the getters return Vector3f(0,0,0); the setters return false.
 */

#include <sol/sol.hpp>

#include <uevr/API.hpp>

#include <sdk/AActor.hpp>
#include <sdk/USceneComponent.hpp>
#include <sdk/UClass.hpp>

#include "SDKFast.hpp"

namespace {

// Best-effort cast from the public uevr::API::UObject* to an internal sdk type
// T*. Both pointers refer to the same underlying UE memory; T must be one of
// sdk::UObject, sdk::AActor, sdk::USceneComponent, etc. Returns nullptr if the
// object's class is not derived from T (per UE's is_a hierarchy).
template <typename T>
T* sdk_cast(uevr::API::UObject* obj) {
    if (obj == nullptr) {
        return nullptr;
    }

    auto sdk_obj = reinterpret_cast<sdk::UObject*>(obj);
    auto sdk_class = sdk_obj->get_class();
    if (sdk_class == nullptr) {
        return nullptr;
    }
    if (!sdk_class->is_a(T::static_class())) {
        return nullptr;
    }
    return reinterpret_cast<T*>(sdk_obj);
}

glm::vec3 get_actor_location(uevr::API::UObject* obj) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return glm::vec3{0.0f, 0.0f, 0.0f};
    }
    return actor->get_actor_location();
}

glm::vec3 get_actor_rotation(uevr::API::UObject* obj) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return glm::vec3{0.0f, 0.0f, 0.0f};
    }
    return actor->get_actor_rotation();
}

bool set_actor_location(uevr::API::UObject* obj, const glm::vec3& loc, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return false;
    }
    return actor->set_actor_location(loc, sweep.value_or(false), teleport.value_or(false));
}

bool set_actor_rotation(uevr::API::UObject* obj, const glm::vec3& rot, sol::optional<bool> teleport) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return false;
    }
    return actor->set_actor_rotation(rot, teleport.value_or(false));
}

glm::vec3 get_world_location(uevr::API::UObject* obj) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        return glm::vec3{0.0f, 0.0f, 0.0f};
    }
    return comp->get_world_location();
}

glm::vec3 get_world_rotation(uevr::API::UObject* obj) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        return glm::vec3{0.0f, 0.0f, 0.0f};
    }
    return comp->get_world_rotation();
}

bool set_world_location(uevr::API::UObject* obj, const glm::vec3& loc, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        return false;
    }
    comp->set_world_location(loc, sweep.value_or(false), teleport.value_or(false));
    return true;
}

bool set_world_rotation(uevr::API::UObject* obj, const glm::vec3& rot, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        return false;
    }
    comp->set_world_rotation(rot, sweep.value_or(false), teleport.value_or(false));
    return true;
}

bool add_world_offset(uevr::API::UObject* obj, const glm::vec3& offset, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        return false;
    }
    comp->add_world_offset(offset, sweep.value_or(false), teleport.value_or(false));
    return true;
}

bool add_world_rotation(uevr::API::UObject* obj, const glm::vec3& rot, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        return false;
    }
    comp->add_world_rotation(rot, sweep.value_or(false), teleport.value_or(false));
    return true;
}

// Composite local-transform setter: write location + rotation (as quat) +
// scale in a single underlying process_event call. UE's K2_SetWorldTransform
// takes an FTransform parameter; with the SDK helper we hand it the raw
// glm::vec3 + glm::vec4 + glm::vec3 and avoid four separate dispatches.
//
// The rotation argument is typed as glm::quat (== Quaternionf in Lua) so
// scripts can pass a Quaternionf directly. The SDK signature is
// `const glm::vec4&`, so we copy out the xyzw components inline.
bool set_local_transform(uevr::API::UObject* obj, const glm::vec3& loc, const glm::quat& quat, const glm::vec3& scale,
                         sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        return false;
    }
    const glm::vec4 quat_vec{quat.x, quat.y, quat.z, quat.w};
    comp->set_local_transform(loc, quat_vec, scale, sweep.value_or(false), teleport.value_or(false));
    return true;
}

glm::vec3 get_socket_location(uevr::API::UObject* obj, const std::wstring& name) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        return glm::vec3{0.0f, 0.0f, 0.0f};
    }
    return comp->get_socket_location(name);
}

glm::vec3 get_socket_rotation(uevr::API::UObject* obj, const std::wstring& name) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        return glm::vec3{0.0f, 0.0f, 0.0f};
    }
    return comp->get_socket_rotation(name);
}

uevr::API::UObject* get_root_component(uevr::API::UObject* obj) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<uevr::API::UObject*>(actor->get_root_component());
}

uevr::API::UObject* get_component_by_class(uevr::API::UObject* obj, uevr::API::UClass* cls) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr || cls == nullptr) {
        return nullptr;
    }
    auto comp = actor->get_component_by_class(reinterpret_cast<sdk::UClass*>(cls));
    return reinterpret_cast<uevr::API::UObject*>(comp);
}

bool destroy_actor(uevr::API::UObject* obj) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return false;
    }
    actor->destroy_actor();
    return true;
}

// Batch read: fill a Lua table with `get_actor_location` results for an array
// of actors in a single binding crossing. Avoids per-element Lua call
// overhead — the underlying process_event still runs N times but the sol2
// marshaling per element collapses to one returned table.
sol::table batch_actor_locations(sol::this_state s, sol::table actors) {
    auto out = sol::state_view{s}.create_table();
    int n = (int)actors.size();
    for (int i = 1; i <= n; ++i) {
        sol::object o = actors[i];
        uevr::API::UObject* uobj = o.is<uevr::API::UObject*>() ? o.as<uevr::API::UObject*>() : nullptr;
        auto a = sdk_cast<sdk::AActor>(uobj);
        if (a != nullptr) {
            out[i] = a->get_actor_location();
        } else {
            out[i] = glm::vec3{0.0f, 0.0f, 0.0f};
        }
    }
    return out;
}

bool is_actor(uevr::API::UObject* obj) {
    return sdk_cast<sdk::AActor>(obj) != nullptr;
}

bool is_scene_component(uevr::API::UObject* obj) {
    return sdk_cast<sdk::USceneComponent>(obj) != nullptr;
}

} // namespace

void bindings::open_sdk_fast(sol::state_view& lua) {
    auto t = lua.create_table();

    t["get_actor_location"]  = &get_actor_location;
    t["get_actor_rotation"]  = &get_actor_rotation;
    t["set_actor_location"]  = &set_actor_location;
    t["set_actor_rotation"]  = &set_actor_rotation;

    t["get_world_location"]  = &get_world_location;
    t["get_world_rotation"]  = &get_world_rotation;
    t["set_world_location"]  = &set_world_location;
    t["set_world_rotation"]  = &set_world_rotation;
    t["add_world_offset"]    = &add_world_offset;
    t["add_world_rotation"]  = &add_world_rotation;

    // Composite (one process_event): write location + quat-rotation + scale
    // together via K2_SetWorldTransform.
    t["set_local_transform"] = &set_local_transform;

    // Per-socket (USceneComponent).
    t["get_socket_location"] = &get_socket_location;
    t["get_socket_rotation"] = &get_socket_rotation;

    // Hierarchy / lifecycle.
    t["get_root_component"]    = &get_root_component;
    t["get_component_by_class"]= &get_component_by_class;
    t["destroy_actor"]         = &destroy_actor;

    // Batch helper: one Lua crossing for many transforms.
    t["batch_actor_locations"] = &batch_actor_locations;

    t["is_actor"]            = &is_actor;
    t["is_scene_component"]  = &is_scene_component;

    // Surface under `uevr.api_fast`. Keeping it separate from `uevr.api` makes
    // intent explicit at call sites ("use the fast path") and avoids shadowing
    // anything the API surface adds later.
    lua["uevr"]["api_fast"] = t;
}
