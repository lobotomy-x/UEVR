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

#include <string>
#include <unordered_map>

#include <sol/sol.hpp>

#include <uevr/API.hpp>

#include <sdk/AActor.hpp>
#include <sdk/USceneComponent.hpp>
#include <sdk/UClass.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/UGameEngine.hpp>
#include <sdk/KismetSystemLibrary.hpp>

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

sdk::USceneComponent* as_component(uevr::API::UObject* obj) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        auto actor = sdk_cast<sdk::AActor>(obj);
        comp = actor != nullptr ? actor->get_root_component() : nullptr;
    }
    return comp;
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
    auto comp = as_component(obj);
    if (comp == nullptr) {
        return glm::vec3{0.0f, 0.0f, 0.0f};
    }
    return comp->get_world_location();
}

glm::vec3 get_world_rotation(uevr::API::UObject* obj) {
    auto comp = as_component(obj);
    if (comp == nullptr) {
        return glm::vec3{0.0f, 0.0f, 0.0f};
    }
    return comp->get_world_rotation();
}

bool set_world_location(uevr::API::UObject* obj, const glm::vec3& loc, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = as_component(obj);
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
    auto comp = as_component(obj);
    if (comp == nullptr) {
        return false;
    }
    comp->add_world_offset(offset, sweep.value_or(false), teleport.value_or(false));
    return true;
}

bool add_world_rotation(uevr::API::UObject* obj, const glm::vec3& rot, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = as_component(obj);
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
    auto comp = as_component(obj);
    if (comp == nullptr) {
        return false;
    }
    const glm::vec4 quat_vec{quat.x, quat.y, quat.z, quat.w};
    comp->set_local_transform(loc, quat_vec, scale, sweep.value_or(false), teleport.value_or(false));
    return true;
}

glm::vec3 get_socket_location(uevr::API::UObject* obj, const std::wstring& name) {
    auto comp = as_component(obj);
    if (comp == nullptr) {
        return glm::vec3{0.0f, 0.0f, 0.0f};
    }
    return comp->get_socket_location(name);
}

glm::vec3 get_socket_rotation(uevr::API::UObject* obj, const std::wstring& name) {
    auto comp = as_component(obj);
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

// Return a flat Lua table of all UActorComponents attached to `actor`. The
// SDK's AActor::get_all_components() does the reflection internally and
// returns a std::vector<UActorComponent*>; we just translate it into a Lua
// table in one binding call instead of forcing scripts through
// actor:get_components() which also goes through the generic reflection
// dispatcher.
sol::table get_all_components(sol::this_state s, uevr::API::UObject* obj) {
    auto out = sol::state_view{s}.create_table();
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return out;
    }
    auto comps = actor->get_all_components();
    int i = 1;
    for (auto* c : comps) {
        out[i++] = reinterpret_cast<uevr::API::UObject*>(c);
    }
    return out;
}

bool is_actor(uevr::API::UObject* obj) {
    return sdk_cast<sdk::AActor>(obj) != nullptr;
}

bool is_scene_component(uevr::API::UObject* obj) {
    return sdk_cast<sdk::USceneComponent>(obj) != nullptr;
}

// ---------------------------------------------------------------------------
// find_class: native port of APIUE.lua's find_fast short-name class lookup.
// Resolves a class by short name ("PlayerController"), engine short name, or
// full object path ("Class /Script/Engine.PlayerController"), with caching.
// Replaces the pure-Lua find_fast + get_unique_short_names + class_short_names
// .json machinery — the short-name->UClass map is built once, in C++, from the
// live UClass set (dedup colliding short names by outer prefix, same as Lua).
// ---------------------------------------------------------------------------
std::unordered_map<std::wstring, uevr::API::UClass*> g_short_name_classes{};
std::unordered_map<std::wstring, uevr::API::UObject*> g_find_cache{};
bool g_short_names_built = false;

void build_short_name_classes() {
    g_short_names_built = true; // set first: even a partial/failed build shouldn't loop
    g_short_name_classes.clear();

    auto class_class = uevr::API::UClass::static_class();
    if (class_class == nullptr) {
        return;
    }

    // Every UClass instance in the process.
    const auto classes = class_class->get_objects_matching(false);
    for (auto* obj : classes) {
        if (obj == nullptr) {
            continue;
        }
        auto* fname = obj->get_fname();
        if (fname == nullptr) {
            continue;
        }
        std::wstring short_name = fname->to_string();
        if (short_name.empty()) {
            continue;
        }

        // On a short-name collision, disambiguate by prefixing the outer's short
        // name ("Engine.PlayerController") — mirrors the Lua behaviour so both
        // the bare and qualified names resolve.
        if (auto existing = g_short_name_classes.find(short_name); existing != g_short_name_classes.end()) {
            if (auto* outer = obj->get_outer(); outer != nullptr) {
                if (auto* outer_fname = outer->get_fname(); outer_fname != nullptr) {
                    g_short_name_classes[outer_fname->to_string() + L'.' + short_name] = (uevr::API::UClass*)obj;
                }
            }
            continue; // keep the first winner for the bare short name
        }

        g_short_name_classes[short_name] = (uevr::API::UClass*)obj;
    }
}

uevr::API::UObject* find_class(const std::wstring& input) {
    if (input.empty()) {
        return nullptr;
    }

    if (!g_short_names_built) {
        build_short_name_classes();
    }

    // 1. Short-name table hit -> resolved UClass directly.
    if (auto it = g_short_name_classes.find(input); it != g_short_name_classes.end()) {
        return it->second;
    }

    // 2. Cache of previously resolved full paths.
    if (auto it = g_find_cache.find(input); it != g_find_cache.end()) {
        return it->second;
    }

    uevr::API::UObject* result = nullptr;

    // 3. If it isn't already a fully-qualified path, try the common Engine path
    //    ("PlayerController" -> "Class /Script/Engine.PlayerController").
    if (input.rfind(L"Class", 0) != 0 && input.rfind(L"ScriptStruct", 0) != 0) {
        const std::wstring engine_input = L"Class /Script/Engine." + input;
        result = uevr::API::get()->find_uobject<uevr::API::UObject>(engine_input.c_str());
    }

    // 4. Otherwise treat the input as a literal object path.
    if (result == nullptr) {
        result = uevr::API::get()->find_uobject<uevr::API::UObject>(input.c_str());
    }

    g_find_cache[input] = result;
    return result;
}

// Force a rebuild of the short-name table (e.g. after new classes are loaded).
void refresh_class_cache() {
    g_short_names_built = false;
    g_find_cache.clear();
    g_short_name_classes.clear();
}

} // namespace

// add_component / get_or_add_component / attach — native ports of the APIUE
// helpers. They accept a short class name (resolved via find_class) so Lua can
// pass "SpringArmComponent" instead of the full "Class /Script/..." path.
static uevr::API::UObject* add_component(uevr::API::UObject* obj, const std::string& class_name, sol::optional<bool> deferred) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return nullptr;
    }
    auto cls = reinterpret_cast<sdk::UClass*>(find_class(std::wstring(class_name.begin(), class_name.end())));
    if (cls == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<uevr::API::UObject*>(actor->add_component_by_class(cls, deferred.value_or(false)));
}

static uevr::API::UObject* get_or_add_component(uevr::API::UObject* obj, const std::string& class_name) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return nullptr;
    }
    auto cls = reinterpret_cast<sdk::UClass*>(find_class(std::wstring(class_name.begin(), class_name.end())));
    if (cls == nullptr) {
        return nullptr;
    }
    if (auto existing = actor->get_component_by_class(cls); existing != nullptr) {
        return reinterpret_cast<uevr::API::UObject*>(existing);
    }
    return reinterpret_cast<uevr::API::UObject*>(actor->add_component_by_class(cls, false));
}

// attach(child, parent, socket?, attach_type?, weld?) — USceneComponent::attach_to.
// attach_type: 0=KeepRelative, 1=KeepWorld, 2=SnapToTarget (EAttachmentRule).
static bool attach(uevr::API::UObject* child, uevr::API::UObject* parent, sol::optional<std::string> socket,
                   sol::optional<int> attach_type, sol::optional<bool> weld) {
    auto child_comp = as_component(child);
    auto parent_comp = as_component(parent);
    if (child_comp == nullptr || parent_comp == nullptr) {
        return false;
    }
    const std::string s = socket.value_or(std::string{"None"});
    return child_comp->attach_to(parent_comp, std::wstring(s.begin(), s.end()), (uint8_t)attach_type.value_or(0), weld.value_or(true));
}

// --- UGameplayStatics / UKismetSystemLibrary native fast paths. The world
// context is fetched internally so Lua callers don't have to pass it. ---
static sdk::UObject* fast_world() {
    auto engine = sdk::UGameEngine::get();
    return engine != nullptr ? reinterpret_cast<sdk::UObject*>(engine->get_world()) : nullptr;
}

static uevr::API::UObject* get_player_controller(sol::optional<int> index) {
    auto ugs = sdk::UGameplayStatics::get();
    auto world = fast_world();
    if (ugs == nullptr || world == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<uevr::API::UObject*>(ugs->get_player_controller(world, index.value_or(0)));
}

static uevr::API::UObject* spawn_object(const std::string& class_name, uevr::API::UObject* outer) {
    auto ugs = sdk::UGameplayStatics::get();
    auto cls = reinterpret_cast<sdk::UClass*>(find_class(std::wstring(class_name.begin(), class_name.end())));
    if (ugs == nullptr || cls == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<uevr::API::UObject*>(ugs->spawn_object(cls, reinterpret_cast<sdk::UObject*>(outer)));
}

static uevr::API::UObject* spawn_actor(const std::string& class_name, const glm::vec3& location) {
    auto ugs = sdk::UGameplayStatics::get();
    auto world = fast_world();
    auto cls = reinterpret_cast<sdk::UClass*>(find_class(std::wstring(class_name.begin(), class_name.end())));
    if (ugs == nullptr || world == nullptr || cls == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<uevr::API::UObject*>(ugs->spawn_actor(world, cls, location));
}

static glm::vec2 world_to_screen(uevr::API::UObject* player_controller, const glm::vec3& world_location) {
    auto ugs = sdk::UGameplayStatics::get();
    auto pc = sdk_cast<sdk::APlayerController>(player_controller);
    if (ugs == nullptr || pc == nullptr) {
        return glm::vec2{0.0f, 0.0f};
    }
    glm::vec3 wl = world_location;
    return ugs->world_to_screen(pc, wl);
}

static void execute_console_command(const std::string& command) {
    auto world = fast_world();
    if (world == nullptr) {
        return;
    }
    sdk::UKismetSystemLibrary::execute_console_command(world, std::wstring(command.begin(), command.end()), nullptr);
}

// --- Read-only reflection / lookup fast paths. All resolve the class via the
// cached short-name table so Lua can pass "PlayerController" instead of a path. ---
static bool is_a(uevr::API::UObject* obj, const std::string& class_name) {
    if (obj == nullptr) {
        return false;
    }
    auto cls = reinterpret_cast<sdk::UClass*>(find_class(std::wstring(class_name.begin(), class_name.end())));
    if (cls == nullptr) {
        return false;
    }
    return reinterpret_cast<sdk::UObject*>(obj)->is_a(cls);
}

static uevr::API::UObject* get_cdo(const std::string& class_name) {
    auto cls = reinterpret_cast<uevr::API::UClass*>(find_class(std::wstring(class_name.begin(), class_name.end())));
    if (cls == nullptr) {
        return nullptr;
    }
    return cls->get_class_default_object();
}

// Enumerate every live instance of a class. Returns a 1-indexed Lua array.
static sol::table get_objects_by_class(sol::this_state s, const std::string& class_name, sol::optional<bool> allow_default) {
    sol::state_view lua{s};
    sol::table out = lua.create_table();
    auto cls = reinterpret_cast<uevr::API::UClass*>(find_class(std::wstring(class_name.begin(), class_name.end())));
    if (cls == nullptr) {
        return out;
    }
    const auto objs = cls->get_objects_matching(allow_default.value_or(false));
    int idx = 1;
    for (auto* o : objs) {
        if (o != nullptr) {
            out[idx++] = o;
        }
    }
    return out;
}

static uevr::API::UObject* get_world() {
    return reinterpret_cast<uevr::API::UObject*>(fast_world());
}

static uevr::API::UObject* get_local_pawn(sol::optional<int> index) {
    auto engine = sdk::UEngine::get();
    if (engine == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<uevr::API::UObject*>(engine->get_localpawn(index.value_or(0)));
}

static uevr::API::UObject* get_local_player(sol::optional<int> index) {
    auto engine = sdk::UEngine::get();
    if (engine == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<uevr::API::UObject*>(engine->get_localplayer(index.value_or(0)));
}

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
    t["get_all_components"]    = &get_all_components;
    t["destroy_actor"]         = &destroy_actor;

    // Component lifecycle by short class name ("SpringArmComponent") + attach.
    t["add_component"]         = &add_component;
    t["get_or_add_component"]  = &get_or_add_component;
    t["attach"]                = &attach;

    // UGameplayStatics / UKismetSystemLibrary fast paths (world context fetched
    // internally; classes accept short names).
    t["get_player_controller"]   = &get_player_controller;
    t["spawn_object"]            = &spawn_object;
    t["spawn_actor"]             = &spawn_actor;
    t["world_to_screen"]         = &world_to_screen;
    t["execute_console_command"] = &execute_console_command;

    // Batch helper: one Lua crossing for many transforms.
    t["batch_actor_locations"] = &batch_actor_locations;

    t["is_actor"]            = &is_actor;
    t["is_scene_component"]  = &is_scene_component;

    // Short-name class lookup (native port of APIUE find_fast). Accepts a short
    // name ("PlayerController"), engine short name, or full path. sol passes a
    // std::string; widen to the wide path the lookup expects.
    t["find_class"] = [](const std::string& name) -> uevr::API::UObject* {
        return find_class(std::wstring(name.begin(), name.end()));
    };
    t["refresh_class_cache"] = &refresh_class_cache;

    // Read-only reflection lookups: short-name class checks + instance/CDO/world
    // accessors (world & engine context fetched internally).
    t["is_a"]                 = &is_a;
    t["get_cdo"]              = &get_cdo;
    t["get_objects_by_class"] = &get_objects_by_class;
    t["get_world"]            = &get_world;
    t["get_local_pawn"]       = &get_local_pawn;
    t["get_local_player"]     = &get_local_player;

    // Surface under `uevr.api_fast`. Keeping it separate from `uevr.api` makes
    // intent explicit at call sites ("use the fast path") and avoids shadowing
    // anything the API surface adds later.
    lua["uevr"]["api_fast"] = t;
}
