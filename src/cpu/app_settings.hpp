#pragma once

#include <map>
#include <filesystem>

#include <GLFW/glfw3.h>
#include <shared/settings.inl>

struct AppSettings {
    std::map<i32, i32> keybinds;
    std::map<i32, i32> mouse_button_binds;
    f32 ui_scl;
    f32 camera_fov;
    f32 mouse_sensitivity;
    f32 render_res_scl;
    std::string world_seed_str;

    SkySettings sky;
    f32vec2 sun_angle;

    bool show_debug_info;
    bool show_console;
    bool show_help;
    bool autosave;
    bool battery_saving_mode;

    void save(std::filesystem::path const &filepath);
    void load(std::filesystem::path const &filepath);
    void clear();
    void reset_default();

    void recompute_sun_direction();
};