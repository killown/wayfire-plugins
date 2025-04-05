#include <algorithm>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <unistd.h>
#include <wayfire/core.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/signal-definitions.hpp>

class wayfire_pixel_color : public wf::per_output_plugin_instance_t {
  wf::plugin_activation_data_t grab_interface = {
      .name = "pixel-color",
      .capabilities = wf::CAPABILITY_GRAB_INPUT,
  };

  wf::wl_timer<false> sample_timer;
  wf::point_t last_sample_pos;
  bool is_sampling = false;
  const int sample_radius = 1; // Radius in pixels around cursor to sample

  // Copies text to clipboard using wl-copy via pipe
  void copy_to_clipboard(const std::string &text) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) {
      LOGE("pipe() failed");
      return;
    }

    pid_t pid = fork();
    if (pid == 0) { // Child process
      close(pipefd[1]);
      if (dup2(pipefd[0], STDIN_FILENO)) {
        _exit(127);
      }
      close(pipefd[0]);
      execlp("wl-copy", "wl-copy", nullptr);
      _exit(127);
    } else if (pid > 0) { // Parent process
      close(pipefd[0]);
      if (write(pipefd[1], text.c_str(), text.size()) != (ssize_t)text.size()) {
        LOGE("Failed to write color to pipe");
      }
      close(pipefd[1]);
    } else {
      LOGE("fork() failed");
      close(pipefd[0]);
      close(pipefd[1]);
    }
  }

  // Gets average RGB color in a square around (center_x, center_y)
  std::array<uint8_t, 3> get_averaged_pixel_color(int center_x, int center_y) {
    auto og = output->get_layout_geometry();

    // Calculate sampling area bounds (clamped to screen edges)
    int start_x = std::max(0, center_x - sample_radius);
    int start_y = std::max(0, center_y - sample_radius);
    int end_x = std::min(og.width - 1, center_x + sample_radius);
    int end_y = std::min(og.height - 1, center_y + sample_radius);

    if (start_x >= og.width || start_y >= og.height || end_x < 0 || end_y < 0) {
      return {0, 0, 0}; // Invalid sampling area
    }

    OpenGL::render_begin();
    GLint old_fb;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);

    auto target = output->render->get_target_framebuffer();
    uint32_t fb = target.fb;
    if (fb == (uint32_t)-1) { // Invalid framebuffer
      OpenGL::render_end();
      return {0, 0, 0};
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fb);

    // Read pixel data from framebuffer
    int width = end_x - start_x + 1;
    int height = end_y - start_y + 1;
    int pixel_count = width * height;

    std::vector<GLubyte> pixels(width * height * 4); // RGBA format
    glReadPixels(start_x, og.height - end_y - 1, width, height, GL_RGBA,
                 GL_UNSIGNED_BYTE, pixels.data());

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
      LOGE("OpenGL error reading pixels: ", err);
      OpenGL::render_end();
      return {0, 0, 0};
    }

    glBindFramebuffer(GL_FRAMEBUFFER, old_fb);
    OpenGL::render_end();

    // Calculate average color values
    unsigned long r_sum = 0, g_sum = 0, b_sum = 0;
    for (int i = 0; i < pixel_count; i++) {
      r_sum += pixels[i * 4];     // Red
      g_sum += pixels[i * 4 + 1]; // Green
      b_sum += pixels[i * 4 + 2]; // Blue
    }

    return {static_cast<uint8_t>(r_sum / pixel_count),
            static_cast<uint8_t>(g_sum / pixel_count),
            static_cast<uint8_t>(b_sum / pixel_count)};
  }

  // Samples color at current cursor position and copies to clipboard
  void sample_color() {
    auto pos = wf::get_core().get_cursor_position();
    auto og = output->get_layout_geometry();
    auto color = get_averaged_pixel_color(pos.x - og.x, pos.y - og.y);

    // Format color as hex string (#RRGGBB)
    char hex_str[8];
    snprintf(hex_str, sizeof(hex_str), "#%02X%02X%02X", color[0], color[1],
             color[2]);

    // Format color as RGB string (R, G, B)
    std::string rgb_str = std::to_string((int)color[0]) + ", " +
                          std::to_string((int)color[1]) + ", " +
                          std::to_string((int)color[2]);

    LOGI("Average color at (", pos.x - og.x, ",", pos.y - og.y,
         "): RGB=", rgb_str, " HEX=", hex_str);

    copy_to_clipboard(hex_str);
    is_sampling = false;
  }

  // Right-click handler to initiate color sampling
  wf::signal::connection_t<
      wf::post_input_event_signal<wlr_pointer_button_event>>
      on_button =
          [=](wf::post_input_event_signal<wlr_pointer_button_event> *ev) {
            if (ev->event->button == BTN_RIGHT &&
                ev->event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
              is_sampling = true;
              last_sample_pos = {(int)wf::get_core().get_cursor_position().x,
                                 (int)wf::get_core().get_cursor_position().y};
              sample_timer.set_timeout(100, [this]() {
                if (is_sampling)
                  sample_color();
                return false;
              });
            } else {
              is_sampling = false;
              sample_timer.disconnect();
            }
          };

  // Motion handler to cancel sampling if cursor moves too far
  wf::signal::connection_t<
      wf::post_input_event_signal<wlr_pointer_motion_event>>
      on_motion =
          [=](wf::post_input_event_signal<wlr_pointer_motion_event> *ev) {
            if (is_sampling) {
              auto new_pos = wf::get_core().get_cursor_position();
              if (hypot(new_pos.x - last_sample_pos.x,
                        new_pos.y - last_sample_pos.y) > 5.0) {
                is_sampling = false;
                sample_timer.disconnect();
              }
            }
          };

public:
  void init() override {
    output->activate_plugin(&grab_interface);
    wf::get_core().connect(&on_button);
    wf::get_core().connect(&on_motion);
  }

  void fini() override {
    output->deactivate_plugin(&grab_interface);
    sample_timer.disconnect();
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_pixel_color>);
