#include <cstdint>
#include <filesystem>

#include "Framework.hpp"
#include "PluginLoader.hpp"
#include "LuaLoader.hpp"

#include <sdk/threading/GameThreadWorker.hpp>

#include <lstate.h> // weird include order because of sol
#include <lgc.h>

#include "bindings/ImGui.hpp"
#include "bindings/FS.hpp"
#include "bindings/Json.hpp"

std::shared_ptr<LuaLoader>& LuaLoader::get() {
    static auto instance = std::make_shared<LuaLoader>();
    return instance;
}

std::optional<std::string> LuaLoader::on_initialize_d3d_thread() {
    // TODO?
    return Mod::on_initialize_d3d_thread();
}

void LuaLoader::on_config_load(const utility::Config& cfg, bool set_defaults) {
    std::scoped_lock _{m_access_mutex};

    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }

    if (m_main_state != nullptr) {
        m_main_state->gc_data_changed(make_gc_data());
    }
}

void LuaLoader::on_config_save(utility::Config& cfg) {
    std::scoped_lock _{m_access_mutex};

    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }


    // TODO: Add config save callback to ScriptState
    if (m_main_state != nullptr) {
        //m_main_state->on_config_save();
    }
}

void LuaLoader::on_frame() {
    // Only run on the game thread
    // on_frame can sometimes run in the DXGI thread, this happens
    // before tick is hooked, which is where the game thread is.
    // once tick is hooked, on_frame will always run on the game thread.
    if (!GameThreadWorker::get().is_same_thread()) {
        return;
    }

    std::scoped_lock _{m_access_mutex};

    if (m_needs_first_reset) {
        spdlog::info("[LuaLoader] Initializing Lua state for the first time...");

        // Calling reset_scripts even though the scripts have never been set yet still works.
        reset_scripts();
        m_needs_first_reset = false;

        spdlog::info("[LuaLoader] Lua state initialized.");
    }

    for (auto state_to_delete : m_states_to_delete) {
        std::erase_if(m_states, [&](std::shared_ptr<ScriptState> state) { return (lua_State*)state->lua().lua_state() == state_to_delete; });
    }

    m_states_to_delete.clear();

    if (m_main_state == nullptr) {
        return;
    }

    for (auto &state : m_states) {
        state->on_frame();
    }
}

void LuaLoader::on_draw_sidebar_entry(std::string_view in_entry) {
    if (in_entry == "Main") {
        if (ImGui::Button("Run script")) {
            OPENFILENAME ofn{};
            char file[260]{};

            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = g_framework->get_window();
            ofn.lpstrFile = file;
            ofn.nMaxFile = sizeof(file);
            ofn.lpstrFilter = "Lua script files (*.lua)\0*.lua\0";
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

            if (GetOpenFileName(&ofn) != FALSE) {
                std::scoped_lock _{ m_access_mutex };
                m_main_state->run_script(file);
                m_loaded_scripts.emplace_back(std::filesystem::path{file}.filename().string());
            }
        }

        ImGui::SameLine();

        if (ImGui::Button("Reset scripts")) {
            reset_scripts();
        }

        ImGui::SameLine();

        if (ImGui::Button("Spawn Debug Console")) {
            if (!m_console_spawned) {
                AllocConsole();
                freopen("CONIN$", "r", stdin);
                freopen("CONOUT$", "w", stdout);
                freopen("CONOUT$", "w", stderr);

                m_console_spawned = true;
            }
        }

        //Garbage collection currently only showing from main lua state, might rework to show total later?
        if (ImGui::TreeNode("Garbage Collection Stats")) {
            std::scoped_lock _{ m_access_mutex };

            auto g = G(m_main_state->lua().lua_state());
            const auto bytes_in_use = g->totalbytes + g->GCdebt;

            ImGui::Text("Megabytes in use: %.2f", (float)bytes_in_use / 1024.0f / 1024.0f);

            ImGui::TreePop();
        }

        if (m_gc_handler->draw("Garbage Collection Handler")) {
            std::scoped_lock _{ m_access_mutex };
            m_main_state->gc_data_changed(make_gc_data());
        }

        if (m_gc_mode->draw("Garbage Collection Mode")) {
            std::scoped_lock _{ m_access_mutex };
            m_main_state->gc_data_changed(make_gc_data());
        }

        if ((uint32_t)m_gc_mode->value() == (uint32_t)ScriptState::GarbageCollectionMode::GENERATIONAL) {
            if (m_gc_minor_multiplier->draw("Minor GC Multiplier")) {
                std::scoped_lock _{ m_access_mutex };
                m_main_state->gc_data_changed(make_gc_data());
            }

            if (m_gc_major_multiplier->draw("Major GC Multiplier")) {
                std::scoped_lock _{ m_access_mutex };
                m_main_state->gc_data_changed(make_gc_data());
            }
        }

        if (m_gc_handler->value() == (int32_t)ScriptState::GarbageCollectionHandler::UEVR_MANAGED) {
            if (m_gc_type->draw("Garbage Collection Type")) {
                std::scoped_lock _{ m_access_mutex };
                m_main_state->gc_data_changed(make_gc_data());
            }

            if ((uint32_t)m_gc_mode->value() != (uint32_t)ScriptState::GarbageCollectionMode::GENERATIONAL) {
                if (m_gc_budget->draw("Garbage Collection Budget")) {
                    std::scoped_lock _{ m_access_mutex };
                    m_main_state->gc_data_changed(make_gc_data());
                }
            }
        }

        m_log_to_disk->draw("Log Lua Errors to Disk");

        auto last_script_error = m_main_state != nullptr ? m_main_state->get_last_script_error() : std::nullopt;

        if (last_script_error.has_value() && !last_script_error->e.empty()) {
            const auto now = std::chrono::system_clock::now();
            const auto diff = now - last_script_error->t;
            const auto sec = std::chrono::duration<float>(diff).count();

            ImGui::TextWrapped("Last Error Time: %.2f seconds ago", sec);

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            ImGui::TextWrapped("Last Script Error: %s", last_script_error->e.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextWrapped("No Script Errors... yet!");
        }

        if (!m_known_scripts.empty()) {
            ImGui::Text("Known scripts:");

            for (auto&& name : m_known_scripts) {
                if (ImGui::Checkbox(name.data(), &m_loaded_scripts_map[name])) {
                    reset_scripts();
                    break;
                }
            }
        } else {
            ImGui::Text("No scripts loaded.");
        }

        ImGui::TreePop();
    }


    if (in_entry == "Script UI") {
        std::scoped_lock _{m_access_mutex};

        if (m_states.empty()) {
            return;
        }

        for (auto& state : m_states) {
            state->on_draw_ui();
        }

        ImGui::TreePop();
    }

    {
        std::scoped_lock __{ m_access_mutex };

        for (auto& entry : m_script_panels) {
            if (in_entry != entry.name) {
                continue;
            }

            if (entry.state.expired()) {
                continue;
            }
    
            auto state = entry.state.lock();
            if (state == nullptr) {
                continue;
            }
    
            std::scoped_lock _{state->context()->get_mutex()};
    
            try {
                state->context()->handle_protected_result(entry.fn());
            } catch (const std::exception& e) {
                state->context()->log_error(std::format("[LuaLoader] Exception in script panel {}: {}", entry.name, e.what()));
            } catch (...) {
                state->context()->log_error(std::format("[LuaLoader] Unknown exception in script panel {}", entry.name));
            }
        }
    }
}

void LuaLoader::reset_scripts() {
    spdlog::info("[LuaLoader] Resetting scripts...");

    std::scoped_lock _{ m_access_mutex };

    if (m_main_state != nullptr) {
        /*auto& mods = g_framework->get_mods()->get_mods();

        for (auto& mod : mods) {
            mod->on_lua_state_destroyed(m_main_state->lua());
        }*/

        m_main_state->on_script_reset();
    }

    m_main_state.reset();
    m_states.clear();
    m_script_panels.clear();

    spdlog::info("[LuaLoader] Destroyed all Lua states.");

    m_main_state = std::make_shared<ScriptState>(make_gc_data(), &g_plugin_initialize_param, true);
    m_states.insert(m_states.begin(), m_main_state);

    for (auto& state : m_states) {
        state_post_init(state);
    }

    //callback functions for main lua state creation
    /*auto& mods = g_framework->get_mods()->get_mods();
    for (auto& mod : mods) {
        mod->on_lua_state_created(m_main_state->lua());
    }*/

    m_loaded_scripts.clear();
    m_known_scripts.clear();

    const auto autorun_path = Framework::get_persistent_dir() / "scripts";
    const auto global_autorun_path = Framework::get_persistent_dir()  / ".." / "UEVR" / "scripts";

    spdlog::info("[LuaLoader] Creating directories {}", autorun_path.string());
    std::filesystem::create_directories(autorun_path);
    spdlog::info("[LuaLoader] Loading scripts...");
    namespace fs = std::filesystem;

	auto load_scripts_from_dir = [this](std::filesystem::path path) {
        if (!fs::exists(path) || !fs::is_directory(path)) {
            return;
        }

		for (auto&& entry : std::filesystem::directory_iterator{path}) {
			auto&& path = entry.path();

			if (path.has_extension() && path.extension() == ".lua") {
				if (!m_loaded_scripts_map.contains(path.filename().string())) {
					m_loaded_scripts_map.emplace(path.filename().string(), true);
				}

				if (m_loaded_scripts_map[path.filename().string()] == true) {
					m_main_state->run_script(path.string());
					m_loaded_scripts.emplace_back(path.filename().string());
				}

				m_known_scripts.emplace_back(path.filename().string());
			}
		}
	};

    load_scripts_from_dir(global_autorun_path);
    load_scripts_from_dir(autorun_path);
    std::sort(m_known_scripts.begin(), m_known_scripts.end());
    std::sort(m_loaded_scripts.begin(), m_loaded_scripts.end());
}

void LuaLoader::state_post_init(std::shared_ptr<ScriptState>& state) {
    std::scoped_lock _{state->context()->get_mutex()};
    auto& lua = state->lua();
    auto lua_table = lua.create_table();

    auto duplicated = false;
    // TODO: Rework this to be within add_additional_bindings somehow without a weak_ptr
    lua_table["add_script_panel"] = [this, &state](sol::this_state s, std::string name, sol::function fn) {
        m_script_panels.emplace_back(PanelEntry{.state = std::weak_ptr<ScriptState>{state}, .name = name, .fn = fn});
        // at present requiring an autorun script with a panel in it will create a duplicate
        // this can also be true for callbacks so this is more of a bandaid fix
        // granted the better solution may be to just put anything with callbacks in a sub folder and require it
        std::sort(m_script_panels.begin(), m_script_panels.end());
        auto comp = [](const PanelEntry& a, const PanelEntry& b) { return (a.name == b.name); };
        auto it = std::unique(m_script_panels.begin(), m_script_panels.end(), comp);
        m_script_panels.erase(it, m_script_panels.end());                                                                          
    };
    

    lua.do_string(R"(
     local short_names
    local function UniqueShortNames()

        local s,r = pcall(function()
            local t = json.load_file("class_short_names.json")
            if #t > 0 then return t end
        end)
        if s then return r end
        base_class = base_class or base_types("Class")
        all_classes = base_class:get_objects_matching(false)
        local short_names = {}
        for i, v in ipairs(all_classes) do
            if v.get_class and v:get_class() == base_class then
                local short_name = v:get_fname():to_string()
                local full_name = v:get_full_name()
                if short_names[short_name] ~= nil
                    then

                   log("Duplicate short name "..short_name.." will be "..v:get_outer():get_short_name().."."..short_name)
                        short_name = v:get_outer():get_short_name().."."..short_name
                end
                short_names[short_name] = full_name
            end
        end
        json.dump_file("class_short_names.json", short_names, 4)
        return short_names
    end
    local _cache = {}
    setmetatable(_cache, {__mode = "v"})
    function uevr.find_class(input)
        assert(type(input) == "string")
        if input:sub(1, 5) ~= "Class" and input:sub(1, 12) ~= "ScriptStruct" then                
            local engine_input =  "Class /Script/Engine.".. input
            if  _cache[engine_input] ~= nil and UEVR_UObjectHook.exists( _cache[engine_input]) then
                return  _cache[engine_input]
            else 
                local temp = uevr.api:find_uobject(engine_input)
                if temp ~= nil and UEVR_UObjectHook.exists(temp) then
                    _cache[engine_input] = temp
                    return _cache[engine_input] 
                end
            end
        end
        if _cache[input] ~= nil and UEVR_UObjectHook.exists( _cache[input])  then
           return _cache[input] 
        else
            local temp = uevr.api:find_uobject(input)                              
              _cache[input] = UEVR_UObjectHook.exists(temp) and temp or nil
       end
       return _cache[input]
    end


    local kismet_libs = {
        Animation =    "Class /Script/AnimGraphRuntime.KismetAnimationLibrary",
        Material =      "Class /Script/Engine.KismetMaterialLibrary",
        Math =           "Class /Script/Engine.KismetMathLibrary",
        Rendering =    "Class /Script/Engine.KismetRenderingLibrary",
        System =        "Class /Script/Engine.KismetSystemLibrary",
        String =          "Class /Script/Engine.KismetStringLibrary",
        Text =            "Class /Script/Engine.KismetTextLibrary",
        StringTable = "Class /Script/Engine.KismetStringTableLibrary",
        Guid =             "Class /Script/Engine.KismetGuidLibrary",
        NodeHelper = "Class /Script/Engine.KismetNodeHelperLibrary",
        }
        Kismet = setmetatable({kismet_cache = {}}, {
                    __call = function(self, lib)
                        self.kismet_cache[lib] = self.kismet_cache[lib] or
                            (kismet_libs[lib] and
                                (UEVR_UObjectHook.get_first_object_by_class(uevr.api:find_uobject(kismet_libs[lib]), true)
                                or uevr.api:find_uobject(kismet_libs[lib]):get_class_default_object()))
                        return self.kismet_cache[lib]
                    end,
                    __index = function(self, lib)
                        return self.kismet_cache[lib] or self(lib)
                })
        uevr.Kismet = Kismet 

        function Statics()
            statics = statics or uevr.api:find_uobject("Class /Script/Engine.GameplayStatics"):get_class_default_object()
            return statics
        end
        uevr.Statics = Statics()

         local function Version()
            local text = Kismet("System"):GetEngineVersion()
            if text:contains("-") then
                text = text:split("-")[1]
            end
            local nums = text:split(".")
            local version = {
                major = tonumber(nums[1]),
                minor = tonumber(nums[2]),
                patch = tonumber(nums[3]),
            }
            return version
        end
        local UE_Version = Version()
        UE5 = UE_Version.major == 5 or false
        UE4 = UE_Version.major == 4 or false
        UE_Version_Minor = tonumber(tostring(UE_Version.minor)..tostring(UE_Version.patch))
  
        Vector3 = UE5 and Vector3d or Vector3f
        Vector4 = UE5 and Vector4d or Vector4f
        Vector2 = UE5 and Vector2d or Vector2f

        Quat = UE5 and Quaterniond or Quaternionf

        function is_array(_table)
	        if _table[1] ~= nil then return true end
        end
        
    function __genOrderedIndex(t)
	    local orderedIndex = {}
	    for key in pairs(t) do
		    -- ensure correct sorting for rotator to Vector3 handling
		    -- idk why its like this but it is
		    if #t == 3 and (type(key) == "string") and (key:lower() == "pitch" or key:lower() == "yaw") then
			    return {"pitch", "yaw", "roll"}
		    end
		    table.insert(t, key)
	    end
	    table.sort(orderedIndex)
	    return orderedIndex
    end



-- pairs does not maintain order. maybe you heard this and thought it was no big deal
-- but its actually borderline unusable. we are fixing that
-- normally this will generate a hidden table with the ordered index based on alphabetical/numeric order
-- but you can instead provide a table with the correct order in the orderedPairs function or directly set __orderedIndex
-- this is crucial for dynamic param-building functions like BreakHitResult which requires empty table values with string keys
function orderedNext(t, state)
	if not t then return end
	local key = (t.__orderedIndex ~= nil and state == nil) and t.__orderedIndex[1] or nil
	if state == nil and t.__orderedIndex == nil then
		-- generate the index the first time
		t.__orderedIndex = __genOrderedIndex(t)
		key = t.__orderedIndex[1]
	else
		-- fetch the next value
		for i = 1, #t.__orderedIndex do
			if t.__orderedIndex[i] == state then
				key = t.__orderedIndex[i + 1]
			end
		end
	end
	if key then
		return key, t[key]
	end
	return
end

-- this is how you actually iterate an ordered table
-- if no orderedIndex exists yet we construct it on the first try
-- you can prebuild your orderedIndex, directly assign it, or pass it here
-- if you want to you can override pairs with orderedPairs in a local variable in your own script
function orderedPairs(t, orderedIndex)
    if keys ~= nil then
		t.__orderedIndex = orderedIndex
    end
	return orderedNext, t, nil
end

-- basically python zip
-- takes two arrays already in correct order and splices them into an orderedTable
function build_ordered_table(keys, values)
	local t = {}
	assert(is_array(keys) and is_array(values))
	for i = 1, #keys do
		t[keys[i]] = values[i]
	end
	t.__orderedIndex = keys
	return t
end

-- this is what I use most of the time
-- very straight forward and simple to use
function ordered_insert(tbl, new_key, new_value)
	tbl.__orderedIndex = tbl.__orderedIndex or {}
	local t = tbl.__orderedIndex
	-- only update insertion order if its new
	if tbl[new_key] == nil then
		table.insert(t, new_key)
	end
	tbl[new_key] = new_value
	return tbl
end


function extend_table(tbl1, tbl2)
  if is_array(tbl1) and is_array(tbl2) then
    for idx, val in ipairs(tbl2) do
        local skip = false
        for _idx, _val in ipairs(tbl1) do
          if _val == val then skip = true end
        end
        if not skip and val ~= nil then
          table.insert(tbl1, val)
        end
      table.insert(tbl1, val)
    end
  else
    for k, v in orderedPairs(tbl2) do
      if not tbl1[k] and v ~= nil then
        tbl1[k] = v
      end
      if tbl1[k] then
        tbl1[k] = v
      end
    end
  end
end

function wipe_table(t)
	while true do
		local k = next(t)
		if not k then break end
		t[k] = nil
	end
end


-- split table into keys and values so you can iterate key names as an array
function break_table(_table)
	local keys, values = {}, {}
	for k, v in orderedPairs(_table) do
		table.insert(keys, k)
		table.insert(values, v)
	end
	return keys, values
end

-- split table into keys and values so you can iterate key names as an array
function take_values(_table)
	if is_array(_table) then return _table end
	local values = {}
	for k, v in orderedPairs(_table) do
		table.insert(values, v)
	end
	return values
end

function can_index(lua_object)
	local mt = getmetatable(lua_object)
	return (not mt and type(lua_object) == "table") or (mt and not not mt.__index)
end
 



function print_to_ue_console(message)
    pc = pc or uevr.api:get_player_controller(0)
    pc:ClientSendMessage(message)
end   

local aactor = uevr.api:find_uobject("Class /Script/Engine.Actor")

function UEVR_UObject:exists()
    return (UEVR_UObjectHook.exists(self) and self) or false
end

 function UEVR_UClass:is_child_of(uclass)
    return Kismet("Math"):ClassIsChildOf(self, uclass)
end

function UEVR_UObject:class_is_child_of(uclass)
     local self_class = self.as_class and self:as_class() or self.get_class and self:get_class()
     return self_class ~=nil and self_class:is_child_of(uclass) or nil
end
  
function UEVR_UObject:add_component(uclass)
    local t = uevr.api:add_component_by_class(
        (self:is_child_class_of(aactor)) or self:get_outer(),
        type(uclass) == "string" and uevr.find_class((uclass:sub(#uclass - 9, #uclass) == "Component") 
        and uclass or uclass.."Component")) 
        or uclass,
        false)
    )
    if not t then print(inspect({uclass, self})) end
    t:K2_SetRelativeTransform(self:as_component():GetRelativeTransform(), false, get_hitresult(), false)
    return t
end

    )");
    


    lua["uevr"]["lua"] = lua_table;
}
/// <summary>
/// Request the creation of a separate script state from the main script state
/// </summary>
/// <returns>the lua state of the new script state</returns>

/// <summary>
/// Request the destruction of the script_state belonging to the lua state in question
/// </summary>


void LuaLoader::add_additional_bindings(sol::state_view& lua) {
    bindings::open_imgui(lua);
    bindings::open_json(lua);
    bindings::open_fs(lua);
}

void LuaLoader::dispatch_event(std::string_view event_name, std::string_view event_data) {
    std::scoped_lock _{m_access_mutex};

    if (m_main_state == nullptr) {
        return;
    }

    m_main_state->dispatch_event(event_name, event_data);
}