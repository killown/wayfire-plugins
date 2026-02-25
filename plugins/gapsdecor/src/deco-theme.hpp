#pragma once
#include "deco-button.hpp"
#include <wayfire/render-manager.hpp>
#include <wayfire/scene-render.hpp>

namespace wf {
namespace decor {
class decoration_theme_t {
public:
  decoration_theme_t();

  int get_title_height() const;
  int get_border_size() const;
  int get_gap_size() const { return gap_size; }
  void set_buttons(button_type_t flags);
  button_type_t button_flags;

  void render_background(const wf::scene::render_instruction_t &data,
                         wf::geometry_t rectangle, bool active) const;

  cairo_surface_t *render_text(std::string text, int width, int height) const;

  struct button_state_t {
    double width;
    double height;
    double border;
    double hover_progress;
  };

  cairo_surface_t *get_button_surface(button_type_t button,
                                      const button_state_t &state) const;

private:
  wf::option_wrapper_t<std::string> font{"gapsdecor/font"};
  wf::option_wrapper_t<int> corner_radius{"gapsdecor/corner_radius"};
  wf::option_wrapper_t<wf::color_t> active_color{"gapsdecor/active_color"};
  wf::option_wrapper_t<wf::color_t> inactive_color{"gapsdecor/inactive_color"};
  wf::option_wrapper_t<wf::color_t> font_color{"gapsdecor/font_color"};
  wf::option_wrapper_t<int> title_height{"gapsdecor/title_height"};
  wf::option_wrapper_t<int> border_size{"gapsdecor/border_size"};
  wf::option_wrapper_t<int> gap_size{"gapsdecor/gap_size"};
};
} // namespace decor
} // namespace wf
