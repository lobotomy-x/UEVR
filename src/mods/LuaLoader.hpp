#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "ScriptContext.hpp"
#include "ScriptState.hpp"

#include "Mod.hpp"
  #include <any>  // don't hate me for this include
using namespace uevr;

class LuaLoader : public Mod {
public:
    static std::shared_ptr<LuaLoader>& get();
    ~LuaLoader();

    std::string_view get_name() const override { return "LuaLoader"; }
    bool is_advanced_mod() const override { return true; }
    std::optional<std::string> on_initialize_d3d_thread() override;

    std::vector<SidebarEntryInfo> get_sidebar_entries() override {
        if (m_script_panels.empty()) {
            return {
                {"Main", true}, {"Script UI", true}, {"Editor", true}
            };
        }

        std::vector<SidebarEntryInfo> entries{
            {"Main", true},
            {"Script UI", true},
            {"Editor", true}
        };

        for (auto& entry : m_script_panels) {
            entries.emplace_back(entry.name, true);
        }

        return entries;
    }

    void on_draw_sidebar_entry(std::string_view in_entry);
    void on_frame() override;

    void on_config_load(const utility::Config& cfg, bool set_defaults) override;
    void on_config_save(utility::Config& cfg) override;


    const auto& get_state() {
        return m_main_state;
    }

    const auto& get_state(int index) { 
        return m_states[index];
    }

    void lock() {
        m_access_mutex.lock();
        for (auto& state : m_states) {
            state->lock();
        }

        ++m_lock_depth;
    }

    void unlock() {
        for (auto& state : m_states) {
            state->unlock();
        }
        m_access_mutex.unlock();

        if (m_lock_depth > 0) {
            --m_lock_depth;
        }
    }
    std::scoped_lock<std::recursive_mutex> get_access_lock() { return std::scoped_lock<std::recursive_mutex>{m_access_mutex}; }
    lua_State* create_state() {
        std::scoped_lock _{m_access_mutex};
        UEVR_PluginInitializeParam* param{};
        m_states.emplace_back(std::make_shared<ScriptState>(make_gc_data(), param,  false));

        for (uint32_t i = 0; i < m_lock_depth; ++i) {
            m_states.back()->lock();
        }

        return ( lua_State*)m_states.back().get()->lua().lua_state();
    }

    void delete_state(lua_State* lua_state) {
        std::scoped_lock _{m_access_mutex};
        m_states_to_delete.push_back(lua_state);
    }

    // Resets the ScriptState and runs autorun scripts again.
    void reset_scripts();
    void state_post_init(std::shared_ptr<ScriptState>& state);
    void add_additional_bindings(sol::state_view& lua);
    void dispatch_event(std::string_view event_name, std::string_view event_data);
      
private:
    ScriptState::GarbageCollectionData make_gc_data() const {
        ScriptState::GarbageCollectionData data{};

        data.gc_handler = (decltype(ScriptState::GarbageCollectionData::gc_handler))m_gc_handler->value();
        data.gc_type = (decltype(ScriptState::GarbageCollectionData::gc_type))m_gc_type->value();
        data.gc_mode = (decltype(ScriptState::GarbageCollectionData::gc_mode))m_gc_mode->value();
        data.gc_budget = std::chrono::microseconds{(uint32_t)m_gc_budget->value()};
        data.gc_minor_multiplier = (uint32_t)m_gc_minor_multiplier->value();
        data.gc_major_multiplier = (uint32_t)m_gc_major_multiplier->value();

        return data;
    }

    std::shared_ptr<ScriptState> m_main_state{};
    std::vector<std::shared_ptr<ScriptState>> m_states{};
    std::recursive_mutex m_access_mutex{};

    // Thread safety for tasks
    std::mutex m_task_mtx{};
    std::vector<sol::protected_function> m_tasks{};
    // Thread safety for data sharing
    std::mutex m_data_mtx{};

public:



    void queue_task(sol::protected_function fn);
    template <typename T> void set_shared(const std::string& key, T val) {
        std::lock_guard<std::mutex> _{m_data_mtx};
        m_shared_data[key] = val;
    }

    template <typename T> T get_shared(const std::string& key) {
        std::lock_guard<std::mutex> _{m_data_mtx};
        if (auto it = m_shared_data.find(key); it != m_shared_data.end()) {
            try {
                return std::any_cast<T>(it->second);
            } catch (...) {
            }
        }
        return T{};
    }

    // Multistate worker threads. spawn_worker creates a named background thread with its own
    // ScriptState; send_to_worker queues a Lua source chunk for that worker to run on its thread;
    // stop_worker signals the worker to drain its queue and exit. Workers communicate back to other
    // states through set_shared/get_shared (the LuaLoader's shared, type-erased data map).
    // Note: workers are intentionally NOT registered in m_states - they're standalone and don't
    // hold the main m_access_mutex, so they can execute in parallel with the main state's frame
    // callbacks. They do NOT expose the full uevr.api binding since most of that surface is not
    // thread-safe.
    bool spawn_worker(const std::string& name, const std::string& bootstrap_source);
    bool send_to_worker(const std::string& name, const std::string& source);
    bool stop_worker(const std::string& name);

private:
    std::unordered_map<std::string, std::any> m_shared_data{};

    struct WorkerState {
        std::thread thread;
        std::mutex queue_mtx;
        std::condition_variable queue_cv;
        std::deque<std::string> queue;
        std::atomic<bool> stop_requested{false};
    };

    std::mutex m_workers_mtx{};
    std::unordered_map<std::string, std::unique_ptr<WorkerState>> m_workers{};
    void worker_thread_main(const std::string& name, const std::string& bootstrap_source);
    std::atomic<uint32_t> m_lock_depth{0};
    // A list of Lua files that have been explicitly loaded either through the user manually loading the script, or
    // because the script was in the autorun directory.
    std::vector<std::string> m_loaded_scripts{};
    std::vector<std::string> m_known_scripts{};
    std::unordered_map<std::string, bool> m_loaded_scripts_map{};
    std::vector<lua_State*> m_states_to_delete{};
    struct PanelEntry {
        std::weak_ptr<uevr::ScriptState> state;
        std::string name;
        sol::protected_function fn;
        // make sortable
        bool operator<(const PanelEntry& other) const { return name < other.name; }
    };
    std::vector<PanelEntry> m_script_panels{};

    bool m_console_spawned{false};
    bool m_needs_first_reset{true};

    const ModToggle::Ptr m_log_to_disk{ ModToggle::create(generate_name("LogToDisk"), false) };

    const ModCombo::Ptr m_gc_handler { 
        ModCombo::create(generate_name("GarbageCollectionHandler"),
        {
            "Managed by UEVR",
            "Managed by Lua"
        }, (int)ScriptState::GarbageCollectionHandler::UEVR_MANAGED)
    };

    const ModCombo::Ptr m_gc_type {
        ModCombo::create(generate_name("GarbageCollectionType"),
        {
            "Step",
            "Full",
        }, (int)ScriptState::GarbageCollectionType::STEP)
    };

    const ModCombo::Ptr m_gc_mode {
        ModCombo::create(generate_name("GarbageCollectionMode"),
        {
            "Generational",
            "Incremental (Mark & Sweep)",
        }, (int)ScriptState::GarbageCollectionMode::GENERATIONAL)
    };

    // Garbage collection budget in microseconds.
    const ModSlider::Ptr m_gc_budget {
        ModSlider::create(generate_name("GarbageCollectionBudget"), 0.0f, 2000.0f, 1000.0f)
    };

    const ModSlider::Ptr m_gc_minor_multiplier {
        ModSlider::create(generate_name("GarbageCollectionMinorMultiplier"), 1.0f, 200.0f, 1.0f)
    };

    const ModSlider::Ptr m_gc_major_multiplier {
        ModSlider::create(generate_name("GarbageCollectionMajorMultiplier"), 1.0f, 1000.0f, 100.0f)
    };

    ValueList m_options{
        *m_log_to_disk,
        *m_gc_handler,
        *m_gc_type,
        *m_gc_mode,
        *m_gc_budget,
        *m_gc_minor_multiplier,
        *m_gc_major_multiplier
    };
};