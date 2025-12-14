#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wayfire/config/config-manager.hpp>
#include <wayfire/config/option.hpp>
#include <wayfire/config/section.hpp>
#include <wayfire/core.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/log.hpp>

namespace fs = std::filesystem;

// Standard Passthrough Vertex Shader
static const char *vertex_shader = R"(
#version 100
attribute highp vec2 position;
attribute highp vec2 uvPosition;
varying highp vec2 uvpos;
void main() {
    gl_Position = vec4(position.xy, 0.0, 1.0);
    uvpos = uvPosition;
}
)";

struct shader_data {
  std::unique_ptr<OpenGL::program_t> program;
  bool needs_time = false;
};

class wayfire_crt_screen : public wf::per_output_plugin_instance_t {
  wf::post_hook_t hook;
  wf::activator_callback toggle_cb;
  wf::activator_callback cycle_cb;
  wf::activator_callback reload_cb;

  // Config Options
  wf::option_wrapper_t<bool> opt_enable{"crt-effect/enabled"};
  wf::option_wrapper_t<std::string> opt_mode{"crt-effect/mode"};
  wf::option_wrapper_t<std::string> opt_shader_path{"crt-effect/shader_path"};
  wf::option_wrapper_t<wf::activatorbinding_t> toggle_key{"crt-effect/toggle"};
  wf::option_wrapper_t<wf::activatorbinding_t> cycle_key{
      "crt-effect/cycle_mode"};
  wf::option_wrapper_t<wf::activatorbinding_t> reload_key{
      "crt-effect/reload_shaders"};
  wf::option_wrapper_t<int> opt_duration{"crt-effect/duration"};

  // Shared Uniforms
  wf::option_wrapper_t<bool> opt_distort{"crt-effect/distortion"};
  wf::option_wrapper_t<bool> opt_scanlines{"crt-effect/scanlines"};
  wf::option_wrapper_t<bool> opt_vignette{"crt-effect/vignette"};
  wf::option_wrapper_t<bool> opt_aberration{"crt-effect/aberration"};

  // Royale/Perfect Specifics
  wf::option_wrapper_t<std::string> r_mask_str{"crt-effect/royale_mask_type"};
  wf::option_wrapper_t<double> r_beam_sigma{"crt-effect/royale_beam_sigma"};
  wf::option_wrapper_t<double> r_scanline_weight{
      "crt-effect/royale_scanline_weight"};
  wf::option_wrapper_t<double> r_border{"crt-effect/royale_border_size"};
  wf::option_wrapper_t<double> r_bright{"crt-effect/royale_brightness"};
  wf::option_wrapper_t<double> r_conv_x_r{"crt-effect/royale_conv_x_r"};
  wf::option_wrapper_t<double> r_conv_x_b{"crt-effect/royale_conv_x_b"};
  wf::option_wrapper_t<double> r_conv_y_r{"crt-effect/royale_conv_y_r"};
  wf::option_wrapper_t<double> r_conv_y_b{"crt-effect/royale_conv_y_b"};

  enum State { INACTIVE, FADE_IN, ACTIVE, FADE_OUT };
  State state = INACTIVE;
  float progression = 0.0f;
  std::chrono::high_resolution_clock::time_point last_frame_time;
  std::chrono::high_resolution_clock::time_point start_time;

  std::map<std::string, shader_data> programs;
  std::vector<std::string> shader_names;

  wf::plugin_activation_data_t grab_interface = {
      .name = "crt",
      .capabilities = 0,
  };

public:
  void init() override {
    if (!wf::get_core().is_gles2()) {
      LOGE("crt: requires GLES2 support");
      return;
    }

    hook = [=](wf::auxilliary_buffer_t &source,
               const wf::render_buffer_t &destination) {
      render(source, destination);
    };

    toggle_cb = [=](auto) {
      // Guard: Do not toggle if no shaders
      if (programs.empty())
        return false;

      if (state == INACTIVE || state == FADE_OUT) {
        if (output->can_activate_plugin(&grab_interface)) {
          if (state == INACTIVE)
            output->render->add_post(&hook);
          state = FADE_IN;
          start_time = std::chrono::high_resolution_clock::now();
          last_frame_time = std::chrono::high_resolution_clock::now();
          output->render->damage_whole();
        }
      } else {
        state = FADE_OUT;
        last_frame_time = std::chrono::high_resolution_clock::now();
        output->render->damage_whole();
      }
      return true;
    };

    cycle_cb = [=](auto) {
      if (shader_names.empty())
        return false;

      std::string current = opt_mode;
      std::string next_mode = shader_names[0];

      auto it = std::find(shader_names.begin(), shader_names.end(), current);
      if (it != shader_names.end() && std::next(it) != shader_names.end()) {
        next_mode = *std::next(it);
      } else if (it != shader_names.end() &&
                 std::next(it) == shader_names.end()) {
        // Wrap around to the first shader if we are at the end
        next_mode = shader_names[0];
      }

      auto section = wf::get_core().config->get_section("crt-effect");
      section->get_option("mode")->set_value_str(next_mode);

      output->render->damage_whole();
      LOGI("CRT: Cycled to mode: ", next_mode);
      return true;
    };

    reload_cb = [=](auto) {
      if (load_shaders()) {
        output->render->damage_whole();
      } else {
        // If reload fails (folder deleted), disable effect
        if (state != INACTIVE) {
          state = INACTIVE;
          output->render->rem_post(&hook);
        }
      }
      return true;
    };

    // SAFETY: Try loading shaders first
    bool loaded = false;
    wf::gles::run_in_context([&] { loaded = load_shaders(); });

    output->add_activator(toggle_key, &toggle_cb);
    output->add_activator(cycle_key, &cycle_cb);
    output->add_activator(reload_key, &reload_cb);

    if (opt_enable && loaded) {
      state = ACTIVE;
      progression = 1.0f;
      output->render->add_post(&hook);
      output->can_activate_plugin(&grab_interface);
    } else if (opt_enable && !loaded) {
      LOGE("CRT: Enabled in config but no shaders found. Disabling.");
    }
  }

  bool load_shaders() {
    std::vector<std::string> search_paths;

    search_paths.push_back((std::string)opt_shader_path);
    search_paths.push_back("~/.local/share/wayfire/crt-effect/shaders");
    search_paths.push_back("/usr/share/wayfire/crt-effect/shaders");

    programs.clear();
    shader_names.clear();

    bool shaders_found = false;

    for (auto &path_str : search_paths) {
      // Expand ~ to HOME if needed
      if (path_str.front() == '~') {
        const char *home = std::getenv("HOME");
        if (home)
          path_str.replace(0, 1, home);
      }

      fs::path shader_dir(path_str);

      // Safety: Check if dir exists
      if (!fs::exists(shader_dir) || !fs::is_directory(shader_dir)) {
        continue;
      }

      LOGI("CRT: Searching for shaders in: ", path_str);

      for (const auto &entry : fs::directory_iterator(shader_dir)) {
        if (entry.path().extension() == ".glsl" ||
            entry.path().extension() == ".frag") {
          std::string name = entry.path().stem().string();

          std::ifstream t(entry.path());
          std::string frag_src((std::istreambuf_iterator<char>(t)),
                               std::istreambuf_iterator<char>());

          // Safety: Empty file check
          if (frag_src.empty())
            continue;

          shader_data data;
          data.program = std::make_unique<OpenGL::program_t>();

          size_t uniforms_start = frag_src.find("// ==UNIFORMS==");
          size_t uniforms_end = frag_src.find("// ==END_UNIFORMS==");

          if (uniforms_start != std::string::npos &&
              uniforms_end != std::string::npos) {
            std::string metadata =
                frag_src.substr(uniforms_start, uniforms_end - uniforms_start);

            if (metadata.find("time: true") != std::string::npos) {
              data.needs_time = true;
            }
          }

          auto program_id =
              OpenGL::compile_program(vertex_shader, frag_src.c_str());

          if (program_id == 0) {
            LOGE("CRT: Failed to compile shader: ", name, " from ", path_str);
            continue;
          }

          data.program->set_simple(program_id);

          if (programs.find(name) == programs.end()) {
            programs[name] = std::move(data);
            shader_names.push_back(name);
            LOGI("CRT: Loaded shader: ", name);
          } else if (path_str == (std::string)opt_shader_path) {
            // Override existing shader with the highest priority config path
            programs[name] = std::move(data);
            LOGI("CRT: Overrode shader: ", name);
          }
          shaders_found = true;
        }
      }
    }

    // Sort shader_names vector to ensure consistent cycle order
    std::sort(shader_names.begin(), shader_names.end());
    // Remove duplicates from shader_names vector (due to overrides)
    shader_names.erase(std::unique(shader_names.begin(), shader_names.end()),
                       shader_names.end());

    if (!shaders_found) {
      LOGE("CRT: No valid .glsl shaders found in any search path.");
      return false;
    }

    // Set the first shader as the current mode if the current mode is invalid
    if (programs.find((std::string)opt_mode) == programs.end() &&
        !shader_names.empty()) {
      auto section = wf::get_core().config->get_section("crt-effect");
      section->get_option("mode")->set_value_str(shader_names[0]);
    }

    return true;
  }

  void render(wf::auxilliary_buffer_t &source,
              const wf::render_buffer_t &destination) {
    // SAFETY GUARD: If no shaders, do not render anything.
    if (programs.empty())
      return;

    auto now = std::chrono::high_resolution_clock::now();
    float dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now - last_frame_time)
                   .count();
    last_frame_time = now;

    float duration = (float)opt_duration;
    if (duration <= 0.1f)
      duration = 1.0f;

    if (state == FADE_IN) {
      progression += dt / duration;
      if (progression >= 1.0f) {
        progression = 1.0f;
        state = ACTIVE;
      } else
        output->render->damage_whole();
    } else if (state == FADE_OUT) {
      progression -= dt / duration;
      if (progression <= 0.0f) {
        progression = 0.0f;
        state = INACTIVE;
        output->render->rem_post(&hook);
        return;
      } else
        output->render->damage_whole();
    }

    std::string mode = opt_mode;

    if (programs.find(mode) == programs.end()) {
      if (!shader_names.empty())
        mode = shader_names[0];
      else
        return;
    }

    // Find the program data for the current mode
    auto &data = programs[mode];

    static const float vertexData[] = {-1.0f, -1.0f, 1.0f,  -1.0f,
                                       1.0f,  1.0f,  -1.0f, 1.0f};
    static const float coordData[] = {0.0f, 0.0f, 1.0f, 0.0f,
                                      1.0f, 1.0f, 0.0f, 1.0f};

    wf::gles::run_in_context([&] {
      wf::gles::bind_render_buffer(destination);

      auto &prog = *data.program;
      prog.use(wf::TEXTURE_TYPE_RGBA);

      GL_CALL(glBindTexture(GL_TEXTURE_2D,
                            wf::gles_texture_t::from_aux(source).tex_id));
      GL_CALL(glActiveTexture(GL_TEXTURE0));

      prog.attrib_pointer("position", 2, 0, vertexData);
      prog.attrib_pointer("uvPosition", 2, 0, coordData);

      prog.uniform2f("resolution", (float)destination.get_size().width,
                     (float)destination.get_size().height);
      prog.uniform1f("anim_progress", progression);

      if (data.needs_time) {
        float time_sec = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - start_time)
                             .count() /
                         1000.0f;
        prog.uniform1f("time", time_sec);
      }

      prog.uniform1i("distort_enable", opt_distort);
      prog.uniform1i("scanlines_enable", opt_scanlines);
      prog.uniform1i("vignette_enable", opt_vignette);
      prog.uniform1i("aberration_enable", opt_aberration);

      int mask_val = 0;
      std::string m = r_mask_str;
      if (m == "slot")
        mask_val = 1;
      else if (m == "dot")
        mask_val = 2;

      prog.uniform1i("r_mask_type", mask_val);
      prog.uniform1f("r_beam_sigma", (float)r_beam_sigma);
      prog.uniform1f("r_scanline_weight", (float)r_scanline_weight);
      prog.uniform1f("r_border_size", (float)r_border);
      prog.uniform1f("r_brightness", (float)r_bright);
      prog.uniform2f("r_convergence_x", (float)r_conv_x_r, (float)r_conv_x_b);
      prog.uniform2f("r_convergence_y", (float)r_conv_y_r, (float)r_conv_y_b);

      GL_CALL(glDisable(GL_BLEND));
      GL_CALL(glDrawArrays(GL_TRIANGLE_FAN, 0, 4));
      GL_CALL(glEnable(GL_BLEND));
      GL_CALL(glBindTexture(GL_TEXTURE_2D, 0));

      prog.deactivate();
    });
  }

  void fini() override {
    if (state != INACTIVE)
      output->render->rem_post(&hook);
    wf::gles::run_in_context_if_gles([&] {
      for (auto &kv : programs) {
        kv.second.program->free_resources();
      }
      programs.clear();
    });
    output->rem_binding(&toggle_cb);
    output->rem_binding(&cycle_cb);
    output->rem_binding(&reload_cb);
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_crt_screen>);
