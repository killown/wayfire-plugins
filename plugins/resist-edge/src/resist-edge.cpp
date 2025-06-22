/*

MIT License

Copyright (c) 2024 Thiago <systemofdown@gmail.com>

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

#include <chrono>
#include <wayfire/core.hpp>
#include <wayfire/input-device.hpp>
#include <wayfire/output-layout.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/signal-provider.hpp>
#include <wayfire/util.hpp>
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
  const int CORNER_WIDTH = 10;        // width of hot region
  const int CORNER_HEIGHT = 50;       // height of hot region
  const double RESISTANCE_FACTOR = 0; // full resistance
  const uint32_t DEBOUNCE_MS = 1000;  // only allow one call per second
  std::chrono::steady_clock::time_point last_exec_time = {};

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
        auto now = std::chrono::steady_clock::now();
        if (last_exec_time == std::chrono::steady_clock::time_point() ||
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_exec_time)
                    .count() >= DEBOUNCE_MS) {

          wf::get_core().run("alacritty");
          last_exec_time = now; // Update last execution time
        }
      }
    }
  }
};

DECLARE_WAYFIRE_PLUGIN(resist_corner_plugin_t);
