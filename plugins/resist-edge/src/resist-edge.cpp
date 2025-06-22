#include <wayfire/core.hpp>
#include <wayfire/input-device.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/signal-provider.hpp>

using namespace wf;

class resist_corner_plugin_t : public plugin_interface_t {
  signal::connection_t<input_event_signal<wlr_pointer_motion_event>>
      motion_connection;

public:
  void init() override {
    motion_connection = [=](input_event_signal<wlr_pointer_motion_event> *ev) {
      handle_pointer_motion(ev);
    };

    get_core().connect(&motion_connection);
  }

  void fini() override {}

private:
  const int CORNER_WIDTH = 3;         // width of hot region
  const int CORNER_HEIGHT = 50;       // height of hot region
  const double RESISTANCE_FACTOR = 0; // full resistance

  void handle_pointer_motion(input_event_signal<wlr_pointer_motion_event> *ev) {
    pointf_t cursor_pos = wf::get_core().get_cursor_position();
    auto output =
        wf::get_core().output_layout->get_output_at(cursor_pos.x, cursor_pos.y);

    if (!output)
      return;

    geometry_t geom = output->get_layout_geometry();

    // Cursor position relative to this output
    double rel_x = cursor_pos.x - geom.x;
    double rel_y = cursor_pos.y - geom.y;

    // Check if inside the top-left hot region
    if ((rel_x >= 0) && (rel_x < CORNER_WIDTH) && (rel_y >= 0) &&
        (rel_y < CORNER_HEIGHT)) {

      // Apply resistance only when moving further into the corner
      if (ev->event->delta_x < 0)
        ev->event->delta_x *= RESISTANCE_FACTOR;

      if (ev->event->delta_y < 0)
        ev->event->delta_y *= RESISTANCE_FACTOR;

      // FIXME: allow to toggle expo or scale
      if (ev->event->delta_x != 0 || ev->event->delta_y != 0) {
        wf::get_core().run("alacritty");
      }
    }
  }
};

DECLARE_WAYFIRE_PLUGIN(resist_corner_plugin_t);
