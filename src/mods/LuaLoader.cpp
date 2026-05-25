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
#include "bindings/SDKFast.hpp"

std::shared_ptr<LuaLoader>& LuaLoader::get() {
    static auto instance = std::make_shared<LuaLoader>();
    return instance;
}

LuaLoader::~LuaLoader() {
    // Tear down worker threads cleanly - std::thread's destructor calls std::terminate if joinable,
    // so we must signal+join each one before m_workers (and the underlying thread objects) destruct.
    std::vector<std::unique_ptr<WorkerState>> to_stop;
    {
        std::scoped_lock _{m_workers_mtx};
        to_stop.reserve(m_workers.size());
        for (auto& [_, w] : m_workers) {
            to_stop.push_back(std::move(w));
        }
        m_workers.clear();
    }
    for (auto& w : to_stop) {
        w->stop_requested.store(true, std::memory_order_release);
        w->queue_cv.notify_all();
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }
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

    std::vector<sol::protected_function> tasks_to_run{};
    {
        std::lock_guard<std::mutex> _{m_task_mtx};
        if (!m_tasks.empty()) {
            tasks_to_run.swap(m_tasks);
        }
    }

    for (auto& fn : tasks_to_run) {
        fn();
    }
}

void LuaLoader::on_draw_sidebar_entry(std::string_view in_entry) {

    const auto autorun_path = Framework::get_persistent_dir() / "scripts";
    const auto global_autorun_path = Framework::get_persistent_dir() / ".." / "UEVR" / "scripts";

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

                 auto script_str = std::filesystem::path{file}.filename().string();
                // allow for easier reload testing
                if (m_loaded_scripts_map[script_str]) {
                    reset_scripts();
                } 
                m_main_state->run_script(file);
               
                m_loaded_scripts.emplace_back(script_str);
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
        static bool all_enabled = true;
        static bool global_enabled = true;
        static bool game_enabled = true;


        if (ImGui::Button("Toggle all scripts")) {
            all_enabled = !all_enabled;
            if (!m_known_scripts.empty()) {     

                for (auto& name : m_known_scripts) {
                    m_loaded_scripts_map[name] = all_enabled;
                }
                reset_scripts();
            }
        }
        if (ImGui::Button("Toggle global scripts")) {  
            if (!m_known_scripts.empty()) {
                global_enabled = !global_enabled;
                for (auto&& name : m_known_scripts) {
                    if (name.contains(std::string_view(global_autorun_path.string()))) {
                     m_loaded_scripts_map[name] = global_enabled;
                    }                 
                }
                reset_scripts();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Toggle game-specific scripts")) {
            if (!m_known_scripts.empty()) {
                game_enabled = !game_enabled;
                for (auto&& name : m_loaded_scripts) {
                    if (name.contains(std::string_view(autorun_path.string()))) {
                        m_loaded_scripts_map[name] = game_enabled;
                    }
                }
                reset_scripts();
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
/*        static bool open = false;
        if (ImGui::Button("Browse for Script")) {
            open = !open;
        }
        if (open) {
            auto get_sorted_entries = [](const std::string& path, bool dirs_first) {
                std::vector<std::pair<std::string, bool>> entries;
                for (const auto& entry : std::filesystem::directory_iterator(path)) {
                    std::string name = entry.path().filename().string();
                    if (entry.path().has_extension() && entry.path().extension() != ".lua")
                        continue;
                    entries.emplace_back(name, entry.is_directory());
                }
                std::sort(entries.begin(), entries.end(), [dirs_first](const auto& a, const auto& b) {
                    if (a.second != b.second)
                        return dirs_first ? a.second > b.second : a.second < b.second;
                    return a.first < b.first;
                });
                return entries;
            };

            const auto activated_key = [](bool is_selected = false) -> bool {
                return ImGui::IsMouseDoubleClicked(0) ||
                       is_selected && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                                          ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown));
            };

            static const std::filesystem::path scripts_path = API::get()->get_persistent_dir(L"scripts");
            static const std::filesystem::path global_path = API::get()->get_persistent_dir(L"..\\UEVR\\scripts");
            static const std::filesystem::path unrealvrmod = API::get()->get_persistent_dir(L"..");

            static const std::filesystem::path downloads = std::filesystem::path(getenv("USERPROFILE")) / "Downloads";
            static std::string current_path = scripts_path.string();
            static std::string filter = "";
            static char filter_buffer[256] = "";
            bool dirs_first = true;
            static bool only_lua = true;
            static int selected_entry = -1;
            static std::string script_path{};
            ImGui::BeginChild("###filebrowser", ImVec2(700, 400), true,
                ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
            // Set focus when opened
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetWindowFocus();
                selected_entry = -1;
            }

            ImGui::Text("Current Path: %s", current_path.c_str());

            // Filter input
            ImGui::InputText("Filter", filter_buffer, sizeof(filter_buffer), ImGuiInputTextFlags_EscapeClearsAll);
            filter = filter_buffer;
            static std::string copy_buffer{};
            // Navigation buttons
            bool can_go_up = std::filesystem::path(current_path).has_parent_path() &&
                             std::filesystem::path(current_path).parent_path().string().find("UnrealVRMod") != std::string::npos;
            if ((can_go_up &&
                    (ImGui::Button("Up") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow))) ||
                ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                current_path = std::filesystem::path(current_path).parent_path().string();
                selected_entry = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Home") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp)) {
                current_path = scripts_path.string();
                selected_entry = -1;
            }
            ImGui::SameLine();
            if (ImGui::Button("Global") || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp)) {
                current_path = global_path.string();
                selected_entry = -1;
            }

            ImGui::BeginChild("FileList", ImVec2(0, 0), true);
            try {
                // Allow navigating to absolute path if entered in filter (for testing)
                if (std::filesystem::path(filter).is_absolute() && std::filesystem::path(filter).has_stem() &&
                    std::filesystem::exists(std::filesystem::path(filter))) {
                    current_path = std::filesystem::path(filter).string();
                    filter.clear();
                    strncpy_s(filter_buffer, filter.c_str(), sizeof(filter_buffer));
                    selected_entry = -1;
                }

                if (ImGui::ArrowButton("##dirs_first", dirs_first ? ImGuiDir_Down : ImGuiDir_Up)) {
                    dirs_first = !dirs_first;
                }
                ImGui::SameLine();
                ImGui::Text(dirs_first ? "Sort Files First" : "Sort Directories First");
                ImGui::Separator();

                std::vector<std::pair<std::string, bool>> entries;
                for (const auto& entry : std::filesystem::directory_iterator(current_path)) {
                    std::string name = entry.path().filename().string();
                    if (only_lua && entry.is_regular_file() && entry.path().extension() != ".lua") {
                        continue;
                    }
                    if (filter.empty() || name.find(filter) != std::string::npos) {
                        entries.emplace_back(name, entry.is_directory());
                    }
                }

                std::sort(entries.begin(), entries.end(), [dirs_first](const auto& a, const auto& b) {
                    if (a.second != b.second)
                        return dirs_first ? a.second > b.second : a.second < b.second;
                    return a.first < b.first;
                });

                // Custom nav inputs - Vr compatible gamepad controls, arrow key movements, or mouse only
                ImGuiIO& io = ImGui::GetIO();
                float scroll_y = ImGui::GetScrollY();
                float scroll_max_y = ImGui::GetScrollMaxY();

                bool no_mouse_input = io.MouseDelta.x == 0.0f && io.MouseDelta.y == 0.0f && !ImGui::IsMouseClicked(0);
                int entry_count = entries.size();
                if (no_mouse_input && entry_count > 0) {
                    static int prev_selected = selected_entry;
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickUp)) {
                        selected_entry = (selected_entry <= 0) ? entry_count - 1 : selected_entry - 1;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown)) {
                        selected_entry = (selected_entry >= entry_count - 1) ? 0 : selected_entry + 1;
                    }

                    selected_entry = std::clamp(selected_entry, -1, entry_count - 1);
                }

                for (int i = 0; i < entries.size(); ++i) {
                    const auto& [name, is_directory] = entries[i];
                    std::string display_name = is_directory ? name + "/" : name;
                    bool is_lua = !is_directory && std::filesystem::path(name).extension() == ".lua";

                    bool is_selected = (i == selected_entry);

                    ImGui::PushID(i);

                    if (is_selected) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f)); // Yellow
                    }

                    if (ImGui::Selectable(display_name.c_str(), is_selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        selected_entry = i;
                        std::filesystem::path entry_path = std::filesystem::path(current_path) / name;
                        if (is_lua)
                            script_path = entry_path.string();
                        ImGui::SetScrollHereY(i / (entries.size() - 1));

                        if (ImGui::IsMouseDoubleClicked(0)) {

                            if (is_directory) {
                                current_path = entry_path.string();
                                selected_entry = -1;
                                script_path.clear();
                            } else if (is_lua) {
                                lua_text = read_file(script_path);

                                text_editor.SetText(lua_text.data());
                                open = false;
                                ImGui::CloseCurrentPopup();
                            }
                        }
                    }

                    if (is_selected && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                                           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown))) {

                        std::filesystem::path entry_path = std::filesystem::path(current_path) / name;
                        if (is_directory) {
                            current_path = entry_path.string();
                            ImGui::SetScrollHereY();
                            selected_entry = -1;
                            script_path.clear();
                        } else if (is_lua) {
                            lua_text = read_file(script_path);

                            text_editor.SetText(lua_text.data());
                            open = false;
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    ImGui::PopStyleColor(is_selected ? 1 : 0);
                    ImGui::PopID();
                }

            } catch (const std::filesystem::filesystem_error& e) {
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: %s", e.what());
            }
            ImGui::EndChild();
            ImGui::EndChild();
            ImGui::EndPopup();
        }*/
        if (!m_known_scripts.empty()) {
            ImGui::Text("Known scripts:");

            for (auto&& name : m_known_scripts) {
                auto path = std::filesystem::path(name.data()).filename().string();
                if (ImGui::Checkbox(path.c_str(), &m_loaded_scripts_map[name])) {
                    reset_scripts();
                    break;
                }
            }
        } else {
            ImGui::Text("No scripts loaded.");
        }
    }


    if (in_entry == "Script UI") {
        std::scoped_lock _{m_access_mutex};

        if (m_states.empty()) {
            return;
        }

        for (auto& state : m_states) {
            state->on_draw_ui();
        }
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
    const static auto autorun_path = Framework::get_persistent_dir() / "scripts";
    const static auto global_autorun_path = Framework::get_persistent_dir() / ".." / "UEVR" / "scripts";
    spdlog::info("[LuaLoader] Resetting scripts...");

    std::scoped_lock _{ m_access_mutex };

    if (m_main_state != nullptr) {
        /*auto& mods = g_framework->get_mods()->get_mods();

        for (auto& mod : mods) {
            mod->on_lua_state_destroyed(m_main_state->lua());
        }*/

        m_main_state->on_script_reset();
    }
    if (!m_states.empty()) {
        for (auto&& state : m_states) {
            state->on_script_reset();
        }
    }

    // CRITICAL: panel entries and queued tasks both hold sol::protected_function refs into the
    // script states' lua_State. sol::protected_function's destructor calls luaL_unref against the
    // registry, so they must run BEFORE we close the lua_State (which happens inside ScriptState's
    // destructor when the last shared_ptr drops). Otherwise luaL_unref dereferences freed memory
    // (lua_rawgeti access violation).
    m_script_panels.clear();
    {
        std::lock_guard<std::mutex> _t{m_task_mtx};
        m_tasks.clear();
    }

    m_main_state.reset();
    m_states.clear();

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
				if (!m_loaded_scripts_map.contains(path.string())) {
					m_loaded_scripts_map.emplace(path.string(), true);
				}

				if (m_loaded_scripts_map[path.string()] == true) {
					m_main_state->run_script(path.string());
					m_loaded_scripts.emplace_back(path.string());
				}

				m_known_scripts.emplace_back(path.string());
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
    //
    lua["uevr"]["set_shared"] =
                    [this](std::string k, sol::object v) {
                        if (v.is<int>())
                            set_shared(k, v.as<int>());
                        else if (v.is<double>())
                            set_shared(k, v.as<double>());
                        else if (v.is<float>())
                            set_shared(k, v.as<float>());
                        else if (v.is<std::string>())
                            set_shared(k, v.as<std::string>());
                        else if (v.is<bool>())
                            set_shared(k, v.as<bool>());
                    };
    lua["uevr"]["get_shared"] = [this](sol::this_state s, std::string k) -> sol::object {
        std::lock_guard<std::mutex> _{m_data_mtx};
        auto it = m_shared_data.find(k);
        if (it == m_shared_data.end()) {
            return sol::make_object(s, sol::lua_nil);
        }
        const auto& val = it->second;
        if (val.type() == typeid(int)) return sol::make_object(s, std::any_cast<int>(val));
        if (val.type() == typeid(double)) return sol::make_object(s, std::any_cast<double>(val));
        if (val.type() == typeid(float)) return sol::make_object(s, std::any_cast<float>(val));
        if (val.type() == typeid(std::string)) return sol::make_object(s, std::any_cast<std::string>(val));
        if (val.type() == typeid(bool)) return sol::make_object(s, std::any_cast<bool>(val));
        return sol::make_object(s, sol::lua_nil);
    };
    lua["uevr"]["run_on_game_thread"] = [this](sol::protected_function fn) {
        queue_task(fn); };

    // Lightweight log channel: routes a Lua string straight to spdlog. Useful
    // for workers (which don't have a redirected stdout, so `print()` goes
    // nowhere visible) and for main-state diagnostics when you want messages
    // to land in the UEVR log file. log_info/warn/error map 1:1 to spdlog
    // severities.
    lua["uevr"]["log_info"]  = [](const std::string& msg) { spdlog::info ("[lua] {}", msg); };
    lua["uevr"]["log_warn"]  = [](const std::string& msg) { spdlog::warn ("[lua] {}", msg); };
    lua["uevr"]["log_error"] = [](const std::string& msg) { spdlog::error("[lua] {}", msg); };

    // Multistate worker thread API.
    // Workers are background threads that own a private ScriptState; scripts can dispatch chunks of
    // Lua source to them via send_to_worker. Communication back happens through set_shared/get_shared
    // (which use the LuaLoader's shared, type-erased map, guarded by m_data_mtx).
    lua["uevr"]["spawn_worker"] = [this](const std::string& name, sol::optional<std::string> bootstrap_source) -> bool {
        return spawn_worker(name, bootstrap_source.value_or(""));
    };
    lua["uevr"]["send_to_worker"] = [this](const std::string& name, const std::string& source) -> bool {
        return send_to_worker(name, source);
    };
    lua["uevr"]["stop_worker"] = [this](const std::string& name) -> bool {
        return stop_worker(name);
    };
    lua["uevr"]["has_worker"] = [this](const std::string& name) -> bool {
        std::scoped_lock _{m_workers_mtx};
        return m_workers.contains(name);
    };

    lua["uevr"]["lua"] = lua_table;

    // SDK fast-path bindings live under `uevr.api_fast` and therefore need
    // `lua["uevr"]` to already exist as a table. add_additional_bindings runs
    // from inside setup_bindings — before ScriptState assigns the returned
    // sol::table to m_lua["uevr"] — so writing to lua["uevr"]["api_fast"]
    // from there panics with "attempt to index a nil value". state_post_init
    // runs after the assignment, so this is the right place for any binding
    // that needs to live under `uevr.*`.
    bindings::open_sdk_fast(lua);
}

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

void LuaLoader::queue_task(sol::protected_function fn) {
    std::lock_guard<std::mutex> _{m_task_mtx};
    m_tasks.push_back(fn);
}

bool LuaLoader::spawn_worker(const std::string& name, const std::string& bootstrap_source) {
    std::scoped_lock _{m_workers_mtx};
    if (m_workers.contains(name)) {
        return false;
    }
    auto worker = std::make_unique<WorkerState>();
    auto* raw = worker.get();
    m_workers.emplace(name, std::move(worker));
    // Start the thread last so the map entry exists before the thread tries to find itself.
    raw->thread = std::thread{[this, name, bootstrap_source]() {
        worker_thread_main(name, bootstrap_source);
    }};
    return true;
}

bool LuaLoader::send_to_worker(const std::string& name, const std::string& source) {
    std::scoped_lock _{m_workers_mtx};
    auto it = m_workers.find(name);
    if (it == m_workers.end()) {
        return false;
    }
    auto& worker = *it->second;
    {
        std::lock_guard<std::mutex> _q{worker.queue_mtx};
        worker.queue.push_back(source);
    }
    worker.queue_cv.notify_one();
    return true;
}

bool LuaLoader::stop_worker(const std::string& name) {
    std::unique_ptr<WorkerState> worker;
    {
        std::scoped_lock _{m_workers_mtx};
        auto it = m_workers.find(name);
        if (it == m_workers.end()) {
            return false;
        }
        worker = std::move(it->second);
        m_workers.erase(it);
    }
    // Flip the stop flag + wake any cv-wait BEFORE handing off — the janitor
    // thread below only needs to clean up after the worker observes these.
    worker->stop_requested.store(true, std::memory_order_release);
    worker->queue_cv.notify_all();

    // Hand the join off to a detached janitor thread so this call returns
    // immediately. The previous implementation joined synchronously, which
    // blocked the calling thread (typically the render thread, since
    // stop_worker is invoked from a script panel button) for the entire
    // remaining duration of whatever Lua chunk the worker was running. A
    // multi-million-iteration benchmark could leave the game frozen for
    // several seconds; long enough that the OS reports it as a crash /
    // not-responding watchdog event. By detaching, we let the worker exit
    // its current Lua chunk, observe stop_requested at the next loop
    // iteration, return from worker_thread_main, and then the janitor
    // joins+destructs the WorkerState. The render thread is never blocked.
    //
    // The janitor takes ownership of the moved unique_ptr; the WorkerState
    // (including its std::thread) outlives this call and is freed only
    // after the worker thread terminates.
    std::thread janitor{[w = std::move(worker)]() mutable {
        if (w->thread.joinable()) {
            w->thread.join();
        }
        // w destructs here on the janitor thread.
    }};
    janitor.detach();
    return true;
}

void LuaLoader::worker_thread_main(const std::string& name, const std::string& bootstrap_source) {
    // Each worker owns its own ScriptState (and therefore its own sol::state) so it can execute Lua
    // without contending with the main script states' execution mutexes. Worker states do NOT see
    // the full uevr.api binding (passing a null PluginInitializeParam keeps ScriptContext from
    // wiring up the unreal/VR API surface, most of which is not thread-safe). Cross-state data goes
    // through uevr.set_shared / uevr.get_shared, which the worker can still call because those bind
    // to LuaLoader's shared map (mutex-protected).
    UEVR_PluginInitializeParam* param = nullptr;
    auto state = std::make_shared<ScriptState>(make_gc_data(), param, false);

    // Re-bind shared-data accessors on the worker's lua state so workers can talk to other states.
    {
        std::scoped_lock _ctx{state->context()->get_mutex()};
        auto& lua = state->lua();
        sol::table uevr_tbl = lua["uevr"].valid() ? lua["uevr"] : lua.create_named_table("uevr");
        uevr_tbl["worker_name"] = name;
        uevr_tbl["set_shared"] = [this](std::string k, sol::object v) {
            if (v.is<int>()) set_shared(k, v.as<int>());
            else if (v.is<double>()) set_shared(k, v.as<double>());
            else if (v.is<float>()) set_shared(k, v.as<float>());
            else if (v.is<std::string>()) set_shared(k, v.as<std::string>());
            else if (v.is<bool>()) set_shared(k, v.as<bool>());
        };
        uevr_tbl["get_shared"] = [this](sol::this_state s, std::string k) -> sol::object {
            std::lock_guard<std::mutex> _{m_data_mtx};
            auto it = m_shared_data.find(k);
            if (it == m_shared_data.end()) return sol::make_object(s, sol::lua_nil);
            const auto& val = it->second;
            if (val.type() == typeid(int)) return sol::make_object(s, std::any_cast<int>(val));
            if (val.type() == typeid(double)) return sol::make_object(s, std::any_cast<double>(val));
            if (val.type() == typeid(float)) return sol::make_object(s, std::any_cast<float>(val));
            if (val.type() == typeid(std::string)) return sol::make_object(s, std::any_cast<std::string>(val));
            if (val.type() == typeid(bool)) return sol::make_object(s, std::any_cast<bool>(val));
            return sol::make_object(s, sol::lua_nil);
        };

        // Worker states get the same log channel as the main state so
        // diagnostic prints from background scripts actually surface
        // somewhere visible (workers have no usable stdout).
        uevr_tbl["log_info"]  = [](const std::string& msg) { spdlog::info ("[lua-worker] {}", msg); };
        uevr_tbl["log_warn"]  = [](const std::string& msg) { spdlog::warn ("[lua-worker] {}", msg); };
        uevr_tbl["log_error"] = [](const std::string& msg) { spdlog::error("[lua-worker] {}", msg); };
    }

    if (!bootstrap_source.empty()) {
        std::scoped_lock _ctx{state->context()->get_mutex()};
        try {
            state->lua().safe_script(bootstrap_source, sol::script_pass_on_error);
        } catch (const std::exception& e) {
            spdlog::error("[LuaLoader] Worker '{}' bootstrap error: {}", name, e.what());
        } catch (...) {
            spdlog::error("[LuaLoader] Worker '{}' bootstrap error: unknown", name);
        }
    }

    // Find our own WorkerState entry by name. It must exist (spawn_worker inserts before starting
    // the thread) - but guard anyway in case stop_worker raced ahead.
    WorkerState* self = nullptr;
    {
        std::scoped_lock _{m_workers_mtx};
        auto it = m_workers.find(name);
        if (it != m_workers.end()) {
            self = it->second.get();
        }
    }
    if (self == nullptr) {
        return;
    }

    while (!self->stop_requested.load(std::memory_order_acquire)) {
        std::string job;
        {
            std::unique_lock<std::mutex> _q{self->queue_mtx};
            self->queue_cv.wait(_q, [self]() {
                return self->stop_requested.load(std::memory_order_acquire) || !self->queue.empty();
            });
            if (self->queue.empty()) {
                continue;
            }
            job = std::move(self->queue.front());
            self->queue.pop_front();
        }

        std::scoped_lock _ctx{state->context()->get_mutex()};
        try {
            state->lua().safe_script(job, sol::script_pass_on_error);
        } catch (const std::exception& e) {
            spdlog::error("[LuaLoader] Worker '{}' error: {}", name, e.what());
        } catch (...) {
            spdlog::error("[LuaLoader] Worker '{}' error: unknown", name);
        }
    }
}
