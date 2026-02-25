#include "deco-theme.hpp"
#include <config.h>
#include <wayfire/core.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/plugins/common/cairo-util.hpp>

namespace wf {
namespace decor {

decoration_theme_t::decoration_theme_t() {}

int decoration_theme_t::get_title_height() const { return title_height; }
int decoration_theme_t::get_border_size() const { return border_size; }
void decoration_theme_t::set_buttons(button_type_t flags) {
  button_flags = flags;
}

static void draw_rounded_rectangle(cairo_t *cr, int x, int y, int w, int h,
                                   double radius) {
  if (radius <= 0) {
    cairo_rectangle(cr, x, y, w, h);
    return;
  }
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - radius, y + radius, radius, -M_PI / 2, 0);
  cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, M_PI / 2);
  cairo_arc(cr, x + radius, y + h - radius, radius, M_PI / 2, M_PI);
  cairo_arc(cr, x + radius, y + radius, radius, M_PI, 270 * M_PI / 180);
  cairo_close_path(cr);
}

void decoration_theme_t::render_background(
    const wf::scene::render_instruction_t &data, wf::geometry_t rectangle,
    bool active) const {
  int radius = corner_radius;
  wf::color_t color = active ? active_color : inactive_color;

  if (radius <= 0) {
    data.pass->add_rect(color, data.target, rectangle, data.damage);
    return;
  }

  auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                            rectangle.width, rectangle.height);
  auto cr = cairo_create(surface);

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_paint(cr);

  cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
  draw_rounded_rectangle(cr, 0, 0, rectangle.width, rectangle.height, radius);
  cairo_fill(cr);

  unsigned char *pixels = cairo_image_surface_get_data(surface);
  int stride = cairo_image_surface_get_stride(surface);
  uint32_t fmt = DRM_FORMAT_ARGB8888;

  auto wlr_tex =
      wlr_texture_from_pixels(wf::get_core().renderer, fmt, stride,
                              rectangle.width, rectangle.height, pixels);

  if (wlr_tex) {
    wf::owned_texture_t background_texture;
    background_texture = wlr_tex;

    data.pass->add_texture(background_texture.get_texture(), data.target,
                           rectangle, data.damage);
  }

  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

cairo_surface_t *decoration_theme_t::render_text(std::string text, int width,
                                                 int height) const {
  auto surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (height == 0)
    return surface;

  wf::color_t color = font_color;
  auto cr = cairo_create(surface);
  const float font_size = height * 0.8;

  PangoFontDescription *font_desc =
      pango_font_description_from_string(((std::string)font).c_str());
  pango_font_description_set_absolute_size(font_desc, font_size * PANGO_SCALE);

  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, font_desc);
  pango_layout_set_text(layout, text.c_str(), text.size());
  cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
  pango_cairo_show_layout(cr, layout);

  pango_font_description_free(font_desc);
  g_object_unref(layout);
  cairo_destroy(cr);

  return surface;
}

cairo_surface_t *
decoration_theme_t::get_button_surface(button_type_t button,
                                       const button_state_t &state) const {
  cairo_surface_t *button_surface = cairo_image_surface_create(
      CAIRO_FORMAT_ARGB32, state.width, state.height);
  auto cr = cairo_create(button_surface);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_rectangle(cr, 0, 0, state.width, state.height);
  cairo_fill(cr);

  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  color_t base = {0.60, 0.60, 0.63, 0.36};
  double line = 0.27;
  double hover = 0.27;

  if (fabs(state.hover_progress) > 1e-3) {
    if (button == BUTTON_CLOSE)
      base = {242.0 / 255.0, 80.0 / 255.0, 86.0 / 255.0, 0.63};
    else if (button == BUTTON_TOGGLE_MAXIMIZE)
      base = {57.0 / 255.0, 234.0 / 255.0, 73.0 / 255.0, 0.63};
    else if (button == BUTTON_MINIMIZE)
      base = {250.0 / 255.0, 198.0 / 255.0, 54.0 / 255.0, 0.63};
    line *= 2.0;
  }

  cairo_set_source_rgba(cr, base.r, base.g, base.b,
                        base.a + hover * state.hover_progress);
  cairo_arc(cr, state.width / 2, state.height / 2, state.width / 2, 0,
            2 * M_PI);
  cairo_fill(cr);

  cairo_set_line_width(cr, state.border);
  cairo_set_source_rgba(cr, 0.00, 0.00, 0.00, line);
  cairo_arc(cr, state.width / 2, state.height / 2,
            state.width / 2 - 0.5 * state.border, 0, 2 * M_PI);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, 0.00, 0.00, 0.00, line / 2);
  cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  if (button == BUTTON_CLOSE) {
    cairo_set_line_width(cr, 1.5 * state.border);
    cairo_move_to(cr, state.width / 4.0, state.height / 4.0);
    cairo_line_to(cr, 3.0 * state.width / 4.0, 3.0 * state.height / 4.0);
    cairo_move_to(cr, 3.0 * state.width / 4.0, state.height / 4.0);
    cairo_line_to(cr, state.width / 4.0, 3.0 * state.height / 4.0);
    cairo_stroke(cr);
  } else if (button == BUTTON_TOGGLE_MAXIMIZE) {
    cairo_set_line_width(cr, 1.5 * state.border);
    cairo_rectangle(cr, state.width / 4.0, state.height / 4.0,
                    state.width / 2.0, state.height / 2.0);
    cairo_stroke(cr);
  } else if (button == BUTTON_MINIMIZE) {
    cairo_set_line_width(cr, 1.75 * state.border);
    cairo_move_to(cr, state.width / 4.0, state.height / 2.0);
    cairo_line_to(cr, 3.0 * state.width / 4.0, state.height / 2.0);
    cairo_stroke(cr);
  }

  cairo_destroy(cr);
  return button_surface;
}

} // namespace decor
} // namespace wf
