/*
 * SDKFast.hpp — bindings that expose UESDK fast-path transform getters
 * and setters to Lua. These bypass the generic reflection dispatcher
 * (UObject __index → prop_to_object → call_function) and the per-call
 * UFunction lookup, going straight to sdk::AActor / sdk::USceneComponent
 * methods that hold the UFunction pointer in a function-local `static`.
 *
 * The win is *not* avoiding the underlying UE process_event call — there
 * is always one of those per read or write — it is avoiding ~3-5 Lua /
 * sol2 / __index round-trips per call, which adds up when a script polls
 * transforms every frame for many actors.
 */

#pragma once

namespace sol {
class state_view;
} // namespace sol

namespace bindings {
void open_sdk_fast(sol::state_view& lua);
}
