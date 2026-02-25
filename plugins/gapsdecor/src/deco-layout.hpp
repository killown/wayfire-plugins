#pragma once

#include "deco-button.hpp"
#include <vector>
#include <wayfire/region.hpp>

namespace wf {
namespace decor {
static constexpr uint32_t DECORATION_AREA_RENDERABLE_BIT = (1 << 16);
static constexpr uint32_t DECORATION_AREA_RESIZE_BIT = (1 << 17);
static constexpr uint32_t DECORATION_AREA_MOVE_BIT = (1 << 18);

enum decoration_area_type_t {
  DECORATION_AREA_MOVE = DECORATION_AREA_MOVE_BIT,
  DECORATION_AREA_TITLE =
      DECORATION_AREA_MOVE_BIT | DECORATION_AREA_RENDERABLE_BIT,
  DECORATION_AREA_BUTTON = DECORATION_AREA_RENDERABLE_BIT,
  DECORATION_AREA_RESIZE_LEFT = WLR_EDGE_LEFT | DECORATION_AREA_RESIZE_BIT,
  DECORATION_AREA_RESIZE_RIGHT = WLR_EDGE_RIGHT | DECORATION_AREA_RESIZE_BIT,
  DECORATION_AREA_RESIZE_TOP = WLR_EDGE_TOP | DECORATION_AREA_RESIZE_BIT,
  DECORATION_AREA_RESIZE_BOTTOM = WLR_EDGE_BOTTOM | DECORATION_AREA_RESIZE_BIT,
};

struct decoration_area_t {
public:
  decoration_area_t(decoration_area_type_t type, wf::geometry_t g);

  decoration_area_t(wf::geometry_t g,
                    std::function<void(wlr_box)> damage_callback,
                    const decoration_theme_t &theme);

  wf::geometry_t get_geometry() const;

  button_t &as_button();

  decoration_area_type_t get_type() const;

private:
  decoration_area_type_t type;
  wf::geometry_t geometry{};

  std::unique_ptr<button_t> button;
};

enum decoration_layout_action_t {
  DECORATION_ACTION_NONE = 0,
  DECORATION_ACTION_MOVE = 1,
  DECORATION_ACTION_RESIZE = 2,
  DECORATION_ACTION_CLOSE = 3,
  DECORATION_ACTION_TOGGLE_MAXIMIZE = 4,
  DECORATION_ACTION_MINIMIZE = 5,
};

class decoration_theme_t;
class decoration_layout_t {
public:
  decoration_layout_t(const decoration_theme_t &theme,
                      std::function<void(wlr_box)> damage_callback);

  void resize(int width, int height);

  std::vector<nonstd::observer_ptr<decoration_area_t>> get_renderable_areas();

  wf::region_t calculate_region() const;

  struct action_response_t {
    decoration_layout_action_t action;
    uint32_t edges;
  };

  action_response_t handle_motion(int x, int y);

  action_response_t handle_press_event(bool pressed = true);

  void handle_focus_lost();

private:
  const int titlebar_size;
  const int border_size;
  const int button_width;
  const int button_height;
  const int button_padding;
  const decoration_theme_t &theme;

  std::function<void(wlr_box)> damage_callback;

  std::vector<std::unique_ptr<decoration_area_t>> layout_areas;

  bool is_grabbed = false;
  wf::point_t grab_origin;
  std::optional<wf::point_t> current_input;
  wf::wl_timer<false> timer;
  bool double_click_at_release = false;

  wf::geometry_t create_buttons(int width, int height);

  uint32_t calculate_resize_edges() const;
  void update_cursor() const;

  nonstd::observer_ptr<decoration_area_t>
  find_area_at(std::optional<wf::point_t> point);

  void unset_hover(std::optional<wf::point_t> position);
  wf::option_wrapper_t<std::string> button_order{"decoration/button_order"};
};
} // namespace decor
} // namespace wf
