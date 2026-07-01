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
#include <sdk/ScriptVector.hpp>

#include "SDKFast.hpp"

namespace {



// CLAUDE PLEASE READ

    /*
        Scan SDK for updates

        Consider if we should add lua AActor and USceneComponent types uobject can cast to with as_

        UNREAL ENGINE UNDER THE HOOD ONLY WORKS WITH SCENECOMPONENT TRANSFORMS AND ATTACHMENTS

        ACTORS ARE JUST ACCESSORS, THIS DOES NOT MATTER FOR SPEED REALISTICALLY BUT ITS SIMPLER TO JUST WORK WITH SCENE COMPS

        On engine tick we should build a fast map cache on a worker thread that gets transforms of any touched scenecomponents or actor->rootcomponents
            any location or rotation reads can read from the cache or add the comp to the cache
            limit reflection calls, reduce potential script conflicts, single unified accessor for all object transforms


        We should add callbacks for the following
                on level
                on world

    */


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

// Actors are data accessors and organizers, scenecomponents are what actually matters for world transformations
// Every attachment or transformation action happens to scenecomponents
// And every call to a location or rotation function is actually just getting the transform and taking a single member

sdk::USceneComponent* as_component(uevr::API::UObject* obj) {
    auto comp = sdk_cast<sdk::USceneComponent>(obj);
    if (comp == nullptr) {
        auto actor = sdk_cast<sdk::AActor>(obj);
        comp = actor != nullptr ? actor->get_root_component() : nullptr;
    }
    return comp;
}

// UE5 uses double-precision FVector (struct size == sizeof(dvec3)). Cached once.
static bool fast_is_ue5() {
    static const bool v = [] {
        auto sv = sdk::ScriptVector::static_struct();
        return sv != nullptr && sv->get_struct_size() == sizeof(glm::dvec3);
    }();
    return v;
}

// Read an FVector/FRotator-returning K2 getter and marshal to the engine-correct
// lua type: Vector3d (glm::dvec3) on UE5, Vector3f (glm::vec3) on UE4. Keeps the
// script-facing type identical to the reflection path (the Vector3 resolver), so
// location()+forward() never mixes float/double. imgui Vector2f/4f stay float.
static sol::object read_vec3_runtime(sol::state_view lua, sdk::UObject* obj, const wchar_t* fn_name) {
    const bool ue5 = fast_is_ue5();
    auto cls = obj != nullptr ? obj->get_class() : nullptr;
    auto func = cls != nullptr ? cls->find_function(fn_name) : nullptr;
    if (func == nullptr) {
        return ue5 ? sol::make_object(lua, glm::dvec3{0.0, 0.0, 0.0})
                   : sol::make_object(lua, glm::vec3{0.0f, 0.0f, 0.0f});
    }
    // The return FVector/FRotator is the only param; buffer = its exact size.
    if (ue5) {
        glm::dvec3 out{0.0, 0.0, 0.0};
        obj->process_event(func, &out);
        return sol::make_object(lua, out);
    }
    glm::vec3 out{0.0f, 0.0f, 0.0f};
    obj->process_event(func, &out);
    return sol::make_object(lua, out);
}

sol::object get_actor_location(sol::this_state s, uevr::API::UObject* obj) {
    sol::state_view lua{s};
    return read_vec3_runtime(lua, reinterpret_cast<sdk::UObject*>(sdk_cast<sdk::AActor>(obj)), L"K2_GetActorLocation");
}

sol::object get_actor_rotation(sol::this_state s, uevr::API::UObject* obj) {
    sol::state_view lua{s};
    return read_vec3_runtime(lua, reinterpret_cast<sdk::UObject*>(sdk_cast<sdk::AActor>(obj)), L"K2_GetActorRotation");
}

// Accept either a Vector3f (glm::vec3) or a Vector3d (glm::dvec3) from Lua and
// narrow to glm::vec3 for the SDK setters. Since the getters now return Vector3d
// on UE5, scripts pass Vector3d straight back into the setters -- without this a
// Vector3d arg would fail to marshal into a glm::vec3 param and throw. (Far-from-
// origin precision on WRITE is preserved by routing through reflection K2_Set*
// in APIUE; these fast setters are the float path.)
static glm::vec3 obj_to_vec3(const sol::object& o) {
    if (o.is<glm::dvec3>()) {
        const auto d = o.as<glm::dvec3>();
        return glm::vec3{(float)d.x, (float)d.y, (float)d.z};
    }
    if (o.is<glm::vec3>()) {
        return o.as<glm::vec3>();
    }
    return glm::vec3{0.0f, 0.0f, 0.0f};
}

bool set_actor_location(uevr::API::UObject* obj, sol::object loc, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return false;
    }
    return actor->set_actor_location(obj_to_vec3(loc), sweep.value_or(false), teleport.value_or(false));
}

bool set_actor_rotation(uevr::API::UObject* obj, sol::object rot, sol::optional<bool> teleport) {
    auto actor = sdk_cast<sdk::AActor>(obj);
    if (actor == nullptr) {
        return false;
    }
    return actor->set_actor_rotation(obj_to_vec3(rot), teleport.value_or(false));
}

sol::object get_world_location(sol::this_state s, uevr::API::UObject* obj) {
    sol::state_view lua{s};
    return read_vec3_runtime(lua, reinterpret_cast<sdk::UObject*>(as_component(obj)), L"K2_GetComponentLocation");
}

sol::object get_world_rotation(sol::this_state s, uevr::API::UObject* obj) {
    sol::state_view lua{s};
    return read_vec3_runtime(lua, reinterpret_cast<sdk::UObject*>(as_component(obj)), L"K2_GetComponentRotation");
}

bool set_world_location(uevr::API::UObject* obj, sol::object loc, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = as_component(obj);
    if (comp == nullptr) {
        return false;
    }
    comp->set_world_location(obj_to_vec3(loc), sweep.value_or(false), teleport.value_or(false));
    return true;
}

bool set_world_rotation(uevr::API::UObject* obj, sol::object rot, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = as_component(obj);
    if (comp == nullptr) {
        return false;
    }
    comp->set_world_rotation(obj_to_vec3(rot), sweep.value_or(false), teleport.value_or(false));
    return true;
}

bool add_world_offset(uevr::API::UObject* obj, sol::object offset, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = as_component(obj);
    if (comp == nullptr) {
        return false;
    }
    comp->add_world_offset(obj_to_vec3(offset), sweep.value_or(false), teleport.value_or(false));
    return true;
}

bool add_world_rotation(uevr::API::UObject* obj, sol::object rot, sol::optional<bool> sweep, sol::optional<bool> teleport) {
    auto comp = as_component(obj);
    if (comp == nullptr) {
        return false;
    }
    comp->add_world_rotation(obj_to_vec3(rot), sweep.value_or(false), teleport.value_or(false));
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
        // this is almost never going to actually occur though
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

    const bool is_bare_short_name = input.rfind(L"Class", 0) != 0 && input.rfind(L"ScriptStruct", 0) != 0;

    // Miss on a bare short name: the one-shot short-name table (built lazily
    // on first use, see g_short_names_built) can be built before this class's
    // module/plugin has finished loading — common for gameplay classes like
    // CameraComponent when find_fast() is called on an early frame. Rebuild
    // once and retry the table lookup before falling through to the fuzzy
    // path-guessing below; this is the difference between resolving the real
    // class vs. never finding it again for the rest of the session (nothing
    // else invalidates this table besides an explicit refresh_class_cache()).
    if (is_bare_short_name) {
        build_short_name_classes();
        if (auto it = g_short_name_classes.find(input); it != g_short_name_classes.end()) {
            g_find_cache[input] = it->second;
            return it->second;
        }
    }

    uevr::API::UObject* result = nullptr;

    // 3. If it isn't already a fully-qualified path, try the common Engine path
    //    ("PlayerController" -> "Class /Script/Engine.PlayerController").
    if (is_bare_short_name) {
        const std::wstring engine_input = L"Class /Script/Engine." + input;
        result = uevr::API::get()->find_uobject<uevr::API::UObject>(engine_input.c_str());
    }

    // 4. Otherwise treat the input as a literal object path.
    if (result == nullptr) {
        result = uevr::API::get()->find_uobject<uevr::API::UObject>(input.c_str());
    }

    // Contract: find_class always resolves to a UClass (it's a class finder,
    // not a general object finder) — every caller either reinterpret_cast's
    // the result straight to UClass* (see add_component/spawn_object/get_cdo
    // below) or, from Lua, calls UClass-only methods like get_objects_matching
    // on it. Step 4's literal-path fallback can, for a bare short name with no
    // "Class "/"ScriptStruct " prefix, land on some other reflected object
    // that merely shares the short name — e.g. many Pawn/Character classes
    // expose their camera through a UObjectProperty literally named
    // "CameraComponent". Returning that silently breaks every caller
    // downstream instead of failing loudly at the source, so refuse to cache
    // or return anything that doesn't actually dcast to UClass.
    if (result != nullptr && result->dcast<uevr::API::UClass>() == nullptr) {
        result = nullptr;
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

// --- Typed property get/set BY NAME (minimal-wrapping fast path). -------------
// Reads/writes a reflected property directly at base+offset, typed by the
// FProperty class name (same switch UObjectHook uses). Covers Bool (bitfield-
// aware), all integer widths, Float/Double, Name (string), and Object (pointer).
// Unsupported types (Str, Struct, Array, Map, Set, Enum) return nil / false --
// use the reflection API for those. No process_event, no sol marshaling of a
// StructObject: one find_property + one memory access per call.

// FFieldClass::get_name() returns a wstring; property type names are ASCII.
static std::string fprop_type_name(uevr::API::FProperty* prop) {
    if (prop == nullptr) {
        return {};
    }
    auto pc = prop->get_class();
    if (pc == nullptr) {
        return {};
    }
    const auto ws = pc->get_name();
    return std::string(ws.begin(), ws.end());
}

// An EnumProperty's value occupies the width of its underlying numeric property
// (UE enums are usually uint8). A fixed int32 read over-reads neighbor bytes and a
// fixed int32 write clobbers up to 3 adjacent live-object bytes, so resolve the
// real width and read/write exactly that.
static int enum_underlying_size(uevr::API::FProperty* prop) {
    auto ep = reinterpret_cast<uevr::API::FEnumProperty*>(prop);
    auto underlying = ep != nullptr ? ep->get_underlying_prop() : nullptr;
    if (underlying == nullptr) {
        return 1;
    }
    const auto un = fprop_type_name(underlying);
    if (un == "Int64Property" || un == "UInt64Property") return 8;
    if (un == "IntProperty"   || un == "UInt32Property") return 4;
    if (un == "Int16Property" || un == "UInt16Property") return 2;
    return 1; // ByteProperty / Int8Property / unknown -> 1 byte (safe default)
}

static sol::object get_property(sol::this_state s, uevr::API::UObject* obj, const std::string& name) {
    sol::state_view lua{s};
    if (obj == nullptr) {
        return sol::make_object(lua, sol::nil);
    }
    auto cls = obj->get_class();
    if (cls == nullptr) {
        return sol::make_object(lua, sol::nil);
    }
    const std::wstring wname(name.begin(), name.end());
    auto prop = cls->find_property(wname);
    if (prop == nullptr) {
        return sol::make_object(lua, sol::nil);
    }
    const auto type = fprop_type_name(prop);
    auto base = reinterpret_cast<uint8_t*>(obj);
    auto at = base + prop->get_offset();

    if (type == "BoolProperty") {
        return sol::make_object(lua, reinterpret_cast<uevr::API::FBoolProperty*>(prop)->get_value_from_object(base));
    }
    if (type == "ByteProperty")   return sol::make_object(lua, (int)*reinterpret_cast<uint8_t*>(at));
    if (type == "Int8Property")   return sol::make_object(lua, (int)*reinterpret_cast<int8_t*>(at));
    if (type == "Int16Property")  return sol::make_object(lua, (int)*reinterpret_cast<int16_t*>(at));
    if (type == "UInt16Property") return sol::make_object(lua, (int)*reinterpret_cast<uint16_t*>(at));
    if (type == "IntProperty")    return sol::make_object(lua, (int)*reinterpret_cast<int32_t*>(at));
    if (type == "UInt32Property") return sol::make_object(lua, (double)*reinterpret_cast<uint32_t*>(at));
    if (type == "Int64Property")  return sol::make_object(lua, (double)*reinterpret_cast<int64_t*>(at));
    if (type == "UInt64Property") return sol::make_object(lua, (double)*reinterpret_cast<uint64_t*>(at));
    if (type == "FloatProperty")  return sol::make_object(lua, *reinterpret_cast<float*>(at));
    if (type == "DoubleProperty") return sol::make_object(lua, *reinterpret_cast<double*>(at));
    if (type == "NameProperty") {
        const auto ws = reinterpret_cast<uevr::API::FName*>(at)->to_string();
        return sol::make_object(lua, std::string(ws.begin(), ws.end()));
    }
    if (type == "ObjectProperty" || type == "ClassProperty") {
        return sol::make_object(lua, *reinterpret_cast<uevr::API::UObject**>(at));
    }
    if (type == "EnumProperty") {
        int64_t v = 0;
        switch (enum_underlying_size(prop)) {
        case 8:  v = *reinterpret_cast<int64_t*>(at); break;
        case 4:  v = *reinterpret_cast<int32_t*>(at); break;
        case 2:  v = *reinterpret_cast<int16_t*>(at); break;
        default: v = *reinterpret_cast<uint8_t*>(at); break;
        }
        return sol::make_object(lua, (int)v);
    }
    if (type == "StrProperty") {
        // FString = { wchar_t* data; int32 count; int32 max } at the offset.
        auto data = *reinterpret_cast<wchar_t**>(at);
        const auto count = *reinterpret_cast<int32_t*>(at + sizeof(void*));
        if (data == nullptr || count <= 0 || count > (1 << 20)) {
            return sol::make_object(lua, std::string());
        }
        const std::wstring ws(data, (size_t)count);
        return sol::make_object(lua, std::string(ws.begin(), ws.end()));
    }
    return sol::make_object(lua, sol::nil);
}

static bool set_property(uevr::API::UObject* obj, const std::string& name, sol::object value) {
    if (obj == nullptr) {
        return false;
    }
    auto cls = obj->get_class();
    if (cls == nullptr) {
        return false;
    }
    const std::wstring wname(name.begin(), name.end());
    auto prop = cls->find_property(wname);
    if (prop == nullptr) {
        return false;
    }
    const auto type = fprop_type_name(prop);
    auto base = reinterpret_cast<uint8_t*>(obj);
    auto at = base + prop->get_offset();

    if (type == "BoolProperty") {
        if (!value.is<bool>()) return false;
        reinterpret_cast<uevr::API::FBoolProperty*>(prop)->set_value_in_object(base, value.as<bool>());
        return true;
    }
    if (type == "ByteProperty")   { if (!value.is<double>()) return false; *reinterpret_cast<uint8_t*>(at)  = (uint8_t)value.as<int>();  return true; }
    if (type == "Int8Property")   { if (!value.is<double>()) return false; *reinterpret_cast<int8_t*>(at)   = (int8_t)value.as<int>();   return true; }
    if (type == "Int16Property")  { if (!value.is<double>()) return false; *reinterpret_cast<int16_t*>(at)  = (int16_t)value.as<int>();  return true; }
    if (type == "UInt16Property") { if (!value.is<double>()) return false; *reinterpret_cast<uint16_t*>(at) = (uint16_t)value.as<int>(); return true; }
    if (type == "IntProperty")    { if (!value.is<double>()) return false; *reinterpret_cast<int32_t*>(at)  = value.as<int32_t>();       return true; }
    if (type == "UInt32Property") { if (!value.is<double>()) return false; *reinterpret_cast<uint32_t*>(at) = (uint32_t)value.as<double>(); return true; }
    if (type == "Int64Property")  { if (!value.is<double>()) return false; *reinterpret_cast<int64_t*>(at)  = (int64_t)value.as<double>();  return true; }
    if (type == "UInt64Property") { if (!value.is<double>()) return false; *reinterpret_cast<uint64_t*>(at) = (uint64_t)value.as<double>(); return true; }
    if (type == "FloatProperty")  { if (!value.is<double>()) return false; *reinterpret_cast<float*>(at)    = (float)value.as<double>();   return true; }
    if (type == "DoubleProperty") { if (!value.is<double>()) return false; *reinterpret_cast<double*>(at)   = value.as<double>();          return true; }
    if (type == "NameProperty") {
        if (!value.is<std::string>()) return false;
        const auto sv = value.as<std::string>();
        *reinterpret_cast<uevr::API::FName*>(at) = uevr::API::FName(std::wstring(sv.begin(), sv.end()));
        return true;
    }
    if (type == "ObjectProperty" || type == "ClassProperty") {
        *reinterpret_cast<uevr::API::UObject**>(at) = value.is<uevr::API::UObject*>() ? value.as<uevr::API::UObject*>() : nullptr;
        return true;
    }
    if (type == "EnumProperty") {
        if (!value.is<double>()) return false;
        const int64_t v = (int64_t)value.as<double>();
        switch (enum_underlying_size(prop)) {
        case 8:  *reinterpret_cast<int64_t*>(at) = v;          break;
        case 4:  *reinterpret_cast<int32_t*>(at) = (int32_t)v; break;
        case 2:  *reinterpret_cast<int16_t*>(at) = (int16_t)v; break;
        default: *reinterpret_cast<uint8_t*>(at) = (uint8_t)v; break;
        }
        return true;
    }
    // StrProperty SET intentionally unsupported: an FString owns a heap buffer;
    // writing a transient Lua-string pointer would dangle. Use reflection for that.
    return false;
}

// --- Fast UFunction call BY NAME (positional scalar args). -------------------
// call_function(obj, "FuncName", arg1, arg2, ...) -> return value (or true on
// void success, nil on failure). Builds the params buffer directly from the
// UFunction's child FProperties, encodes positional args by type, process_event,
// decodes the return param. Same type coverage as get/set_property (bool, ints,
// float/double, name-string, object). Struct/array/out params unsupported --
// args at those positions are left zeroed; use reflection for those signatures.

// Encode one sol value into a params-buffer slot at `at` (propbase-relative, so
// Bool uses the propbase bitfield variant). Returns false on type mismatch.
static bool encode_param(uevr::API::FProperty* prop, uint8_t* at, sol::object v) {
    const auto type = fprop_type_name(prop);
    if (type == "BoolProperty") {
        if (!v.is<bool>()) return false;
        reinterpret_cast<uevr::API::FBoolProperty*>(prop)->set_value_in_propbase(at, v.as<bool>());
        return true;
    }
    if (type == "ByteProperty")   { if (!v.is<double>()) return false; *reinterpret_cast<uint8_t*>(at)  = (uint8_t)v.as<int>();  return true; }
    if (type == "Int8Property")   { if (!v.is<double>()) return false; *reinterpret_cast<int8_t*>(at)   = (int8_t)v.as<int>();   return true; }
    if (type == "Int16Property")  { if (!v.is<double>()) return false; *reinterpret_cast<int16_t*>(at)  = (int16_t)v.as<int>();  return true; }
    if (type == "UInt16Property") { if (!v.is<double>()) return false; *reinterpret_cast<uint16_t*>(at) = (uint16_t)v.as<int>(); return true; }
    if (type == "IntProperty")    { if (!v.is<double>()) return false; *reinterpret_cast<int32_t*>(at)  = v.as<int32_t>();       return true; }
    if (type == "UInt32Property") { if (!v.is<double>()) return false; *reinterpret_cast<uint32_t*>(at) = (uint32_t)v.as<double>(); return true; }
    if (type == "Int64Property")  { if (!v.is<double>()) return false; *reinterpret_cast<int64_t*>(at)  = (int64_t)v.as<double>();  return true; }
    if (type == "UInt64Property") { if (!v.is<double>()) return false; *reinterpret_cast<uint64_t*>(at) = (uint64_t)v.as<double>(); return true; }
    if (type == "FloatProperty")  { if (!v.is<double>()) return false; *reinterpret_cast<float*>(at)    = (float)v.as<double>();   return true; }
    if (type == "DoubleProperty") { if (!v.is<double>()) return false; *reinterpret_cast<double*>(at)   = v.as<double>();          return true; }
    if (type == "NameProperty") {
        if (!v.is<std::string>()) return false;
        const auto sv = v.as<std::string>();
        *reinterpret_cast<uevr::API::FName*>(at) = uevr::API::FName(std::wstring(sv.begin(), sv.end()));
        return true;
    }
    if (type == "ObjectProperty" || type == "ClassProperty") {
        *reinterpret_cast<uevr::API::UObject**>(at) = v.is<uevr::API::UObject*>() ? v.as<uevr::API::UObject*>() : nullptr;
        return true;
    }
    return false;
}

// Decode a params-buffer slot at `at` into a sol value (return/out param read).
static sol::object decode_param(sol::state_view lua, uevr::API::FProperty* prop, uint8_t* at) {
    const auto type = fprop_type_name(prop);
    if (type == "BoolProperty")   return sol::make_object(lua, reinterpret_cast<uevr::API::FBoolProperty*>(prop)->get_value_from_propbase(at));
    if (type == "ByteProperty")   return sol::make_object(lua, (int)*reinterpret_cast<uint8_t*>(at));
    if (type == "Int8Property")   return sol::make_object(lua, (int)*reinterpret_cast<int8_t*>(at));
    if (type == "Int16Property")  return sol::make_object(lua, (int)*reinterpret_cast<int16_t*>(at));
    if (type == "UInt16Property") return sol::make_object(lua, (int)*reinterpret_cast<uint16_t*>(at));
    if (type == "IntProperty")    return sol::make_object(lua, (int)*reinterpret_cast<int32_t*>(at));
    if (type == "UInt32Property") return sol::make_object(lua, (double)*reinterpret_cast<uint32_t*>(at));
    if (type == "Int64Property")  return sol::make_object(lua, (double)*reinterpret_cast<int64_t*>(at));
    if (type == "UInt64Property") return sol::make_object(lua, (double)*reinterpret_cast<uint64_t*>(at));
    if (type == "FloatProperty")  return sol::make_object(lua, *reinterpret_cast<float*>(at));
    if (type == "DoubleProperty") return sol::make_object(lua, *reinterpret_cast<double*>(at));
    if (type == "NameProperty") {
        const auto ws = reinterpret_cast<uevr::API::FName*>(at)->to_string();
        return sol::make_object(lua, std::string(ws.begin(), ws.end()));
    }
    if (type == "ObjectProperty" || type == "ClassProperty") {
        return sol::make_object(lua, *reinterpret_cast<uevr::API::UObject**>(at));
    }
    return sol::make_object(lua, sol::nil);
}

static sol::object call_function(sol::this_state s, uevr::API::UObject* obj, const std::string& name, sol::variadic_args va) {
    sol::state_view lua{s};
    if (obj == nullptr) {
        return sol::make_object(lua, sol::nil);
    }
    auto cls = obj->get_class();
    if (cls == nullptr) {
        return sol::make_object(lua, sol::nil);
    }
    const std::wstring wname(name.begin(), name.end());
    auto fn = cls->find_function(wname);
    if (fn == nullptr) {
        return sol::make_object(lua, sol::nil);
    }

    // Collect in-params (positional) + the single return param, in declaration order.
    std::vector<uevr::API::FProperty*> in_params;
    uevr::API::FProperty* return_prop = nullptr;
    for (auto field = fn->get_child_properties(); field != nullptr; field = field->get_next()) {
        auto pc = field->get_class();
        if (pc == nullptr) continue;
        const auto cn = pc->get_name();
        if (std::wstring(cn).find(L"Property") == std::wstring::npos) continue;
        auto prop = reinterpret_cast<uevr::API::FProperty*>(field);
        if (prop->is_return_param()) { return_prop = prop; continue; }
        if (prop->is_out_param() && !prop->is_reference_param()) continue; // pure out: skip
        if (!prop->is_param()) continue;
        in_params.push_back(prop);
    }

    const size_t params_size = std::max<size_t>(64, (size_t)fn->get_properties_size());
    std::vector<uint8_t> params(params_size, 0);

    size_t ai = 0;
    for (auto prop : in_params) {
        sol::object arg = (ai < va.size()) ? va[ai] : sol::object(sol::lua_nil);
        if (!arg.valid() || arg == sol::lua_nil) { ai++; continue; } // leave zeroed
        encode_param(prop, params.data() + prop->get_offset(), arg);
        ai++;
    }

    fn->call(obj, params.data());

    if (return_prop != nullptr) {
        return decode_param(lua, return_prop, params.data() + return_prop->get_offset());
    }
    return sol::make_object(lua, true); // void success
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

    // Typed property get/set by name (no process_event, no StructObject wrap).
    // Scalars/bool/name/object only; nil/false for struct/array/str/enum.
    t["get_property"]         = &get_property;
    t["set_property"]         = &set_property;

    // Fast UFunction call by name with positional scalar args + return value.
    t["call_function"]        = &call_function;

    // Surface under `uevr.api_fast`. Keeping it separate from `uevr.api` makes
    // intent explicit at call sites ("use the fast path") and avoids shadowing
    // anything the API surface adds later.
    lua["uevr"]["api_fast"] = t;
}
