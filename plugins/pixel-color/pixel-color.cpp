#include <algorithm>
#include <fcntl.h> // For O_CLOEXEC
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
  const int sample_radius = 1; // Samples 3x3 area

  void copy_to_clipboard(const std::string &text) {
    // Create a pipe with O_CLOEXEC to prevent FD leaks
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) == -1) {
      LOGE("pipe() failed");
      return;
    }

    pid_t pid = fork();
    if (pid == 0) { // Child process
      // Close the write end of the pipe
      close(pipefd[1]);

      // Redirect stdin from the pipe
      if (dup2(pipefd[0], STDIN_FILENO) == -1) {
        _exit(127);
      }
      close(pipefd[0]);

      // Execute wl-copy
      execlp("wl-copy", "wl-copy", nullptr);
      _exit(127);
    } else if (pid > 0) { // Parent process
      // Close the read end of the pipe
      close(pipefd[0]);

      // Write the data and close the pipe
      if (write(pipefd[1], text.c_str(), text.size()) != (ssize_t)text.size()) {
        LOGE("Failed to write color to pipe");
      }
      close(pipefd[1]);

      // Don't wait for child (non-blocking)
      LOGI("Copied color to clipboard: ", text);
    } else {
      LOGE("fork() failed");
      close(pipefd[0]);
      close(pipefd[1]);
    }
  }

  std::array<uint8_t, 3> get_averaged_pixel_color(int center_x, int center_y) {
    auto og = output->get_layout_geometry();

    // Clamp sampling area to within screen bounds
    int start_x = std::max(0, center_x - sample_radius);
    int start_y = std::max(0, center_y - sample_radius);
    int end_x = std::min(og.width - 1, center_x + sample_radius);
    int end_y = std::min(og.height - 1, center_y + sample_radius);

    if (start_x >= og.width || start_y >= og.height || end_x < 0 || end_y < 0) {
      return {0, 0, 0};
    }

    OpenGL::render_begin();

    // Save current GL state
    GLint old_fb;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);

    // Get the output's framebuffer
    auto target = output->render->get_target_framebuffer();
    uint32_t fb = target.fb;
    if (fb == (uint32_t)-1) {
      OpenGL::render_end();
      return {0, 0, 0};
    }

    // Bind to the output's framebuffer
    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, fb));

    // Calculate sample area dimensions
    int width = end_x - start_x + 1;
    int height = end_y - start_y + 1;
    int pixel_count = width * height;

    // Read the entire sample area at once (using RGBA format)
    std::vector<GLubyte> pixels(width * height * 4);
    GL_CALL(glReadPixels(start_x, og.height - end_y - 1, width, height, GL_RGBA,
                         GL_UNSIGNED_BYTE, pixels.data()));

    // Check for OpenGL errors
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
      LOGE("OpenGL error reading pixels: ", err);
      OpenGL::render_end();
      return {0, 0, 0};
    }

    // Restore previous framebuffer
    GL_CALL(glBindFramebuffer(GL_FRAMEBUFFER, old_fb));
    OpenGL::render_end();

    // Calculate average color
    unsigned long r_sum = 0, g_sum = 0, b_sum = 0;
    for (int i = 0; i < pixel_count; i++) {
      r_sum += pixels[i * 4];
      g_sum += pixels[i * 4 + 1];
      b_sum += pixels[i * 4 + 2];
    }

    return {static_cast<uint8_t>(r_sum / pixel_count),
            static_cast<uint8_t>(g_sum / pixel_count),
            static_cast<uint8_t>(b_sum / pixel_count)};
  }

  void sample_color() {
    auto pos = wf::get_core().get_cursor_position();
    auto og = output->get_layout_geometry();
    auto color = get_averaged_pixel_color(pos.x - og.x, pos.y - og.y);

    // Format the color string
    char hex_str[8];
    snprintf(hex_str, sizeof(hex_str), "#%02X%02X%02X", color[0], color[1],
             color[2]);

    std::string rgb_str = std::to_string((int)color[0]) + ", " +
                          std::to_string((int)color[1]) + ", " +
                          std::to_string((int)color[2]);

    // Log and copy to clipboard
    LOGI("Average color at (", pos.x - og.x, ",", pos.y - og.y,
         "): RGB=", rgb_str, " HEX=", hex_str);

    copy_to_clipboard(hex_str); // Copy hex format to clipboard
    is_sampling = false;
  }

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
    LOGI("Pixel color plugin initialized - Right-click to pick colors");
    LOGI("Sampling area: ", (sample_radius * 2 + 1), "x",
         (sample_radius * 2 + 1), " pixels");
  }

  void fini() override {
    output->deactivate_plugin(&grab_interface);
    sample_timer.disconnect();
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_pixel_color>);
