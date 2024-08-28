#pragma once

#include "deco-button.hpp"
#include <vector>
#include <wayfire/region.hpp>

namespace wf {
namespace decor {
static constexpr uint32_t GAPSDECOR_AREA_RENDERABLE_BIT = (1 << 16);
static constexpr uint32_t GAPSDECOR_AREA_RESIZE_BIT = (1 << 17);
static constexpr uint32_t GAPSDECOR_AREA_MOVE_BIT = (1 << 18);

/** Different types of areas around the gapsdecor */
enum gapsdecor_area_type_t {
  GAPSDECOR_AREA_MOVE = GAPSDECOR_AREA_MOVE_BIT,
  GAPSDECOR_AREA_TITLE =
      GAPSDECOR_AREA_MOVE_BIT | GAPSDECOR_AREA_RENDERABLE_BIT,
  GAPSDECOR_AREA_BUTTON = GAPSDECOR_AREA_RENDERABLE_BIT,
  GAPSDECOR_AREA_RESIZE_LEFT = WLR_EDGE_LEFT | GAPSDECOR_AREA_RESIZE_BIT,
  GAPSDECOR_AREA_RESIZE_RIGHT = WLR_EDGE_RIGHT | GAPSDECOR_AREA_RESIZE_BIT,
  GAPSDECOR_AREA_RESIZE_TOP = WLR_EDGE_TOP | GAPSDECOR_AREA_RESIZE_BIT,
  GAPSDECOR_AREA_RESIZE_BOTTOM = WLR_EDGE_BOTTOM | GAPSDECOR_AREA_RESIZE_BIT,
};

/**
 * Represents an area of the gapsdecor which reacts to input events.
 */
struct gapsdecor_area_t {
public:
  /**
   * Initialize a new gapsdecor area with the given type and geometry
   */
  gapsdecor_area_t(gapsdecor_area_type_t type, wf::geometry_t g);

  /**
   * Initialize a new gapsdecor area holding a button.
   *
   * @param g The geometry of the button.
   * @param damage_callback Callback to execute when button needs repaint.
   * @param theme The theme to use for the button.
   */
  gapsdecor_area_t(wf::geometry_t g,
                   std::function<void(wlr_box)> damage_callback,
                   const gapsdecor_theme_t &theme);

  /** @return The geometry of the gapsdecor area, relative to the layout */
  wf::geometry_t get_geometry() const;

  /** @return The area's button, if the area is a button. Otherwise UB */
  button_t &as_button();

  /** @return The type of the gapsdecor area */
  gapsdecor_area_type_t get_type() const;

private:
  gapsdecor_area_type_t type;
  wf::geometry_t geometry{};

  /* For buttons only */
  std::unique_ptr<button_t> button;
};

/**
 * Action which needs to be taken in response to an input event
 */
enum gapsdecor_layout_action_t {
  GAPSDECOR_ACTION_NONE = 0,
  /* Drag actions */
  GAPSDECOR_ACTION_MOVE = 1,
  GAPSDECOR_ACTION_RESIZE = 2,
  /* Button actions */
  GAPSDECOR_ACTION_CLOSE = 3,
  GAPSDECOR_ACTION_TOGGLE_MAXIMIZE = 4,
  GAPSDECOR_ACTION_MINIMIZE = 5,
};

class gapsdecor_theme_t;
/**
 * Manages the layout of the gapsdecors, i.e positioning of the title,
 * buttons, etc.
 *
 * Also dispatches the input events to the appropriate place.
 */
class gapsdecor_layout_t {
public:
  /**
   * Create a new gapsdecor layout for the given theme.
   * When the theme changes, the gapsdecor layout needs to be created again.
   *
   * @param damage_callback The function to be called when a part of the
   * layout needs a repaint.
   */
  gapsdecor_layout_t(const gapsdecor_theme_t &theme,
                     std::function<void(wlr_box)> damage_callback);

  /** Regenerate layout using the new size */
  void resize(int width, int height);

  /**
   * @return The gapsdecor areas which need to be rendered, in top to bottom
   *  order.
   */
  std::vector<nonstd::observer_ptr<gapsdecor_area_t>> get_renderable_areas();

  /** @return The combined region of all layout areas */
  wf::region_t calculate_region() const;

  struct action_response_t {
    gapsdecor_layout_action_t action;
    /* For resizing action, determine the edges for resize request */
    uint32_t edges;
  };

  /** Handle motion event to (x, y) relative to the gapsdecor */
  action_response_t handle_motion(int x, int y);

  /**
   * Handle press or release event.
   * @param pressed Whether the event is a press(true) or release(false)
   *  event.
   * @return The action which needs to be carried out in response to this
   *  event.
   */
  action_response_t handle_press_event(bool pressed = true);

  /**
   * Handle focus lost event.
   */
  void handle_focus_lost();

private:
  const int titlebar_size;
  const int border_size;
  const int button_width;
  const int button_height;
  const int button_padding;
  const gapsdecor_theme_t &theme;

  std::function<void(wlr_box)> damage_callback;

  std::vector<std::unique_ptr<gapsdecor_area_t>> layout_areas;

  bool is_grabbed = false;
  /* Position where the grab has started */
  wf::point_t grab_origin;
  /* Last position of the input */
  std::optional<wf::point_t> current_input;
  /* double-click timer */
  wf::wl_timer<false> timer;
  bool double_click_at_release = false;

  /** Create buttons in the layout, and return their total geometry */
  wf::geometry_t create_buttons(int width, int height);

  /** Calculate resize edges based on @current_input */
  uint32_t calculate_resize_edges() const;
  /** Update the cursor based on @current_input */
  void update_cursor() const;

  /**
   * Find the layout area at the given coordinates, if any
   * @return The layout area or null on failure
   */
  nonstd::observer_ptr<gapsdecor_area_t>
  find_area_at(std::optional<wf::point_t> point);

  /** Unset hover state of hovered button at @position, if any */
  void unset_hover(std::optional<wf::point_t> position);
  wf::option_wrapper_t<std::string> button_order{"gapsdecor/button_order"};
};
} // namespace decor
} // namespace wf
