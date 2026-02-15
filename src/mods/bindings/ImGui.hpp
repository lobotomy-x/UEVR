/*
This license governs this file (ImGui.hpp), and is separate from the license for the rest of the UEVR codebase.

The MIT License

Copyright (c) 2023-2025 praydog

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

namespace uevr {
class ScriptState;
} // namespace uevr

namespace sol {
class state_view;
}

namespace bindings {
void open_imgui(sol::state_view& s);
}         
//
//// Expose imgui API to C++ plugins
//#include <imgui.h>
//#include <sol/sol.hpp>
//#include <string>
//
//namespace api::imgui {
///*// Core helpers
//void cleanup();
//ImVec2 create_imvec2(sol::object obj);
//ImVec4 create_imvec4(sol::object obj);
//ImVec4 create_imvec4_color(sol::object obj);
//ImU32 create_imu32_color(sol::object obj);
//
//// Widgets
//bool button(const char* label, sol::object size_object, sol::object flags_object);
//bool small_button(const char* label);
//bool invisible_button(const char* id, sol::object size_object, sol::object flags_object);
//bool arrow_button(const char* str_id, int dir);
//sol::variadic_results combo(sol::this_state s, const char* label, sol::object selection, sol::table values);
//sol::variadic_results checkbox(sol::this_state s, const char* label, bool v);
//sol::variadic_results drag_float(sol::this_state s, const char* label, float v, float v_speed, float v_min, float v_max,
//    const char* display_format, sol::object flags_object);
//sol::variadic_results drag_float2(sol::this_state s, const char* label, sol::object v, float v_speed, float v_min, float v_max,
//    const char* display_format, sol::object flags_object);
//sol::variadic_results drag_float3(sol::this_state s, const char* label, sol::object v, float v_speed, float v_min, float v_max,
//    const char* display_format, sol::object flags_object);
//sol::variadic_results drag_float4(sol::this_state s, const char* label, sol::object v, float v_speed, float v_min, float v_max,
//    const char* display_format, sol::object flags_object);
//sol::variadic_results drag_int(sol::this_state s, const char* label, int v, float v_speed, int v_min, int v_max,
//    const char* display_format, sol::object flags_object);
//sol::variadic_results slider_float(sol::this_state s, const char* label, float v, float v_min, float v_max,
//    const char* display_format, sol::object flags_object);
//sol::variadic_results slider_int(sol::this_state s, const char* label, int v, int v_min, int v_max, const char* display_format, sol::object flags_object);
//
//// Text
//void text(const char* text, sol::object flags_object);
//void text_colored(const char* text, sol::object color);
//void text_wrapped(const char* text);
//
//// Drag & drop
//bool begin_drag_drop_source(sol::object flags_object);
//void end_drag_drop_source();
//bool set_drag_drop_payload(const char* type, sol::object data);
//bool is_payload_accepted();
//sol::object accept_payload(sol::this_state s, const char* type, sol::object flags_object);
//bool begin_drag_drop_target();
//void end_drag_drop_target();
//void render_drag_drop(sol::object rect_start, sol::object rect_end);
//void accept_drag_drop(const char* type);
//
//// Windows and layout
//bool begin_window(const char* name, sol::object open_obj, ImGuiWindowFlags flags = 0);
//void end_window();
//bool begin_child_window(const char* name, sol::object size_obj, sol::object border_obj, ImGuiWindowFlags flags = 0);
//void end_child_window();
//bool begin_popup(const char* str_id, sol::object flags_obj);
//bool begin_popup_modal(const char* str_id, sol::object open_obj, sol::object flags_obj);
//void end_popup();
//void open_popup(const char* str_id, sol::object flags_obj);
//
//// Styling
//void push_style_color(int style_color, sol::object color_obj);
//void pop_style_color(sol::object count_obj);
//void push_style_var(int idx, sol::object value_obj);
//void pop_style_var(sol::object count_obj);
//
//// ID and focus
//void push_id(sol::object id);
//void pop_id();
//ImGuiID get_id(sol::object id);
//ImGuiID get_active_id();
//ImGuiID get_hovered_id();
//ImGuiID get_item_id();
//void active_item_by_id(sol::object id);
//void clear_active_id();
//
//// Input state
//Vector2f get_mouse();
//int get_key_index(int imgui_key);
//bool is_key_down(int key);
//bool is_key_pressed(int key);
//bool is_key_released(int key);
//bool is_mouse_down(int button);
//bool is_mouse_clicked(int button);
//bool is_mouse_released(int button);
//bool is_mouse_double_clicked(int button);
//
//// Misc
//void focus_window();
//void focus_item();
//void set_tooltip(const char* text);
//const char* get_clipboard();
//void set_clipboard(sol::object data);
//Vector2f calc_text_size(const char* text, sol::object text_end, sol::object double_hash_hides_text, sol::object wrap_width);
//Vector2f get_window_size();
//Vector2f get_window_pos();
//Vector2f get_content_region_available();
//
//// Tables and columns
//bool begin_table(const char* str_id, int column, sol::object flags_obj, sol::object outer_size_obj, sol::object inner_width_obj);
//void end_table();
//void table_next_row(sol::object row_flags, sol::object min_row_height);
//bool table_next_column();
//bool table_set_column_index(int column_index);
//void table_setup_column(const char* label, sol::object flags_obj, sol::object init_width_or_weight_obj, sol::object user_id_obj);
//ImGuiTableSortSpecs* table_get_sort_specs();
//
//// Other helper APIs
//void push_item_width(float item_width);
//void pop_item_width();
//void set_next_item_width(float item_width);
//float calc_item_width();
//void item_size(sol::object pos, sol::object size, sol::object text_baseline_y);
//bool item_add(const char* label, sol::object pos, sol::object size);
//bool is_item_hovered(sol::object flags_obj);
//bool is_item_active();
//bool is_item_focused();
//bool is_item_clicked();
//bool is_item_edited();
//bool is_item_visible();
//bool is_any_item_hovered();
//bool is_item_toggled_selection();
//bool is_any_item_active();
//bool is_any_item_focused();
//bool is_item_toggled_open();
//
//// Font & demo
//int load_font(sol::object filepath_obj, int size, sol::object ranges);
//void push_font(int font);
//void pop_font();
//int get_default_font_size();
//void show_metrics_window(bool* enabled);
//void show_font_atlas();
//void show_demo_window(bool* enabled);
//void show_font_selector(const char* label);
//void show_debug_log_window(bool* enabled);
//void show_stack_tool_window(bool* enabled);
//
//} // namespace api::imgui
//
//namespace api::draw {
//// Draw list helpers
//void text(const char* text, float x, float y, ImU32 color);
//void filled_rect(float x, float y, float w, float h, ImU32 color);
//void outline_rect(float x, float y, float w, float h, ImU32 color);
//void line(float x1, float y1, float x2, float y2, ImU32 color);
//void outline_circle(float x, float y, float radius, ImU32 color, sol::object num_segments);
//void filled_circle(float x, float y, float radius, ImU32 color, sol::object num_segments);
//void outline_quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, ImU32 color);
//void filled_quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, ImU32 color);
//void outline_triangle(float x1, float y1, float x2, float y2, float x3, float y3, ImU32 color);
//void filled_triangle(float x1, float y1, float x2, float y2, float x3, float y3, ImU32 color);
//void outline_polyline(sol::object points, ImU32 color, float thickness);
//void closed_polyline(sol::object points, ImU32 color, float thickness);
//void draw_line(Vector2f p1, Vector2f p2, ImU32 color, float thickness);
//void draw_rect(Vector2f pos, Vector2f size, ImU32 color, float rounding, float thickness);
//void draw_filled_rect(Vector2f pos, Vector2f size, ImU32 color, float rounding);
//void draw_circle(Vector2f center, float radius, ImU32 color, int num_segments, float thickness);
//void draw_filled_circle(Vector2f center, float radius, ImU32 color, int num_segments);
//void draw_ngon(Vector2f center, float radius, ImU32 color, int num_segments, float thickness);
//void draw_filled_ngon(Vector2f center, float radius, ImU32 color, int num_segments);
//void draw_bezier_curve(Vector2f p1, Vector2f p2, Vector2f p3, ImU32 color, float thickness);
//void draw_quad(Vector2f p1, Vector2f p2, Vector2f p3, Vector2f p4, ImU32 color, float thickness);
//void draw_filled_quad(Vector2f p1, Vector2f p2, Vector2f p3, Vector2f p4, ImU32 color);*/
//} // namespace api::draw
//
