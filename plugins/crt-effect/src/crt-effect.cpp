#include <chrono>
#include <wayfire/config/config-manager.hpp>
#include <wayfire/config/option.hpp>
#include <wayfire/config/section.hpp>
#include <wayfire/core.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/log.hpp>

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

// DISCRETE MODE
static const char *frag_discrete = R"(
#version 100
precision highp float;
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform bool distort_enable;
uniform bool scanlines_enable;
uniform bool vignette_enable;
uniform bool aberration_enable;
uniform float anim_progress;
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.02, 1.02);
    uv.x *= 1.0 + pow((abs(uv.y) / 5.0), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 4.0), 2.0);
    return (uv / 2.0) + 0.5;
}
void main() {
    vec2 flat_uv = uvpos;
    vec2 curved_uv = flat_uv;
    if (distort_enable) curved_uv = curve(flat_uv);
    vec2 uv = mix(flat_uv, curved_uv, anim_progress);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 color;
    if (aberration_enable) {
        float offset = 0.001 * anim_progress; 
        color.r = texture2D(smp, uv + vec2(offset, 0.0)).r;
        color.g = texture2D(smp, uv).g;
        color.b = texture2D(smp, uv - vec2(offset, 0.0)).b;
    } else {
        color = texture2D(smp, uv).rgb;
    }
    if (scanlines_enable) {
        float scan_cnt = resolution.y * 0.5;
        float scan = sin(uv.y * scan_cnt * 3.14159 * 2.0);
        color *= 1.0 - (0.15 * anim_progress) * (0.5 - 0.5 * scan);
        float grill = sin(uv.x * resolution.x * 0.333 * 3.14159 * 2.0);
        color *= 1.0 - (0.1 * anim_progress) * (0.5 - 0.5 * grill);
    }
    if (vignette_enable) {
        float vig = 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y);
        color *= mix(1.0, pow(vig, 0.15), anim_progress);
    }
    color *= 1.0 + (0.2 * anim_progress);
    gl_FragColor = vec4(color, 1.0);
}
)";

// RETRO MODE
static const char *frag_retro = R"(
#version 100
precision highp float;
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform highp float time; 
uniform bool distort_enable;
uniform float anim_progress;
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= 1.1;    
    uv.x *= 1.0 + pow((abs(uv.y) / 5.0), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 4.0), 2.0);
    uv  = (uv / 2.0) + 0.5;
    uv =  uv * 0.92 + 0.04;
    return uv;
}
void main() {
    vec2 uv = uvpos;
    if (distort_enable) uv = curve(uv);
    vec3 col;
    float x = sin(0.3 * time + uv.y * 21.0) * sin(0.7 * time + uv.y * 29.0) * sin(0.3 + 0.33 * time + uv.y * 31.0) * 0.0017;
    col.r = texture2D(smp, vec2(x + uv.x + 0.001, uv.y + 0.001)).x + 0.05;
    col.g = texture2D(smp, vec2(x + uv.x + 0.000, uv.y - 0.002)).y + 0.05;
    col.b = texture2D(smp, vec2(x + uv.x - 0.002, uv.y + 0.000)).z + 0.05;
    float vig = (0.0 + 1.0 * 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y));
    col *= vec3(pow(vig, 0.3));
    col *= vec3(0.95, 1.05, 0.95) * 2.8;
    float scans = clamp(0.35 + 0.35 * sin(3.5 * time + uv.y * resolution.y * 1.5), 0.0, 1.0);
    float s = pow(scans, 1.7);
    col = col * vec3(0.4 + 0.7 * s);
    if (distort_enable) {
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) col *= 0.0;
    }
    col *= anim_progress;
    gl_FragColor = vec4(col, 1.0);
}
)";

// ROYALE LITE
static const char *frag_royale = R"(
#version 100
precision highp float;
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform float anim_progress;
uniform int r_mask_type; 
uniform float r_beam_sigma;
uniform float r_border_size;
uniform float r_scanline_weight;
uniform float r_brightness; 
uniform vec2 r_convergence_x; 
uniform vec2 r_convergence_y;
uniform bool distort_enable;
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.0 + r_border_size); 
    uv.x *= 1.0 + pow((abs(uv.y) / 4.5), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 3.5), 2.0);
    return (uv / 2.0) + 0.5;
}
void main() {
    vec2 uv = uvpos;
    if (distort_enable) uv = curve(uv);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 uv_r = uv + vec2(r_convergence_x.x, r_convergence_y.x);
    vec2 uv_g = uv;
    vec2 uv_b = uv + vec2(r_convergence_x.y, r_convergence_y.y);
    vec3 col;
    col.r = texture2D(smp, uv_r).r;
    col.g = texture2D(smp, uv_g).g;
    col.b = texture2D(smp, uv_b).b;
    col = pow(col, vec3(2.2));
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    float scan_pos = fract(uv.y * resolution.y * 0.5) - 0.5; 
    float sigma = r_beam_sigma + (lum * 0.1); 
    float beam = exp(-(scan_pos * scan_pos) / (2.0 * sigma * sigma));
    col *= 1.0 - (r_scanline_weight * (1.0 - beam));
    float px = uv.x * resolution.x;
    float py = uv.y * resolution.y;
    float mask = 1.0;
    if (r_mask_type == 0) mask = 0.85 + 0.15 * sin(px * 3.14159 * 1.5); 
    else if (r_mask_type == 1) {
        float odd = mod(floor(py / 4.0), 2.0); 
        float slot = sin((px + odd * 1.5) * 3.14159 * 1.5);
        float vert = sin(py * 3.14159);
        mask = 0.8 + 0.2 * (slot * vert);
    } else mask = 0.8 + 0.2 * (sin(px * 3.0) * sin(py * 3.0));
    col *= mask;
    col = pow(col, vec3(1.0 / 2.2)); 
    col *= r_brightness; 
    col *= anim_progress;
    gl_FragColor = vec4(col, 1.0);
}
)";

// PERFECT (ARCADE/GAMING)
static const char *frag_perfect = R"(
#version 100
precision highp float;
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform float anim_progress;
uniform bool distort_enable;
uniform float r_brightness; 
const float HARD_SCAN = -8.0; 
const vec3 MASK_DARK = vec3(0.5, 0.5, 0.5);
const vec3 MASK_LIGHT = vec3(1.2, 1.2, 1.2);
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.01, 1.01);
    uv.x *= 1.0 + pow((abs(uv.y) / 4.8), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 3.8), 2.0);
    return (uv / 2.0) + 0.5;
}
vec4 get_tex_smart(sampler2D s, vec2 uv, vec2 res) {
    vec2 p = uv * res;
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); 
    vec2 uv_smart = (i + f + 0.5) / res;
    return texture2D(s, uv_smart);
}
float scanline_gaussian(float uv_y, float res_y) {
    float scan_pos = fract(uv_y * res_y * 0.5);
    float dist = scan_pos - 0.5;
    return exp(HARD_SCAN * dist * dist);
}
vec3 slot_mask(vec2 pos) {
    float px = pos.x;
    float py = pos.y;
    float row = floor(py / 3.0); 
    float stagger = mod(row, 2.0) * 1.5; 
    float mask_val = sin((px + stagger) * 3.14159 * 1.2);
    mask_val = smoothstep(-0.5, 0.5, mask_val);
    return mix(MASK_DARK, MASK_LIGHT, mask_val);
}
void main() {
    vec2 uv = uvpos;
    if (distort_enable) uv = curve(uv);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 col = get_tex_smart(smp, uv, resolution).rgb;
    col = pow(col, vec3(2.4)); 
    float scan = scanline_gaussian(uv.y, resolution.y);
    col *= mix(0.6, 1.1, scan);
    vec3 mask = slot_mask(uv * resolution);
    col *= mask;
    col = pow(col, vec3(1.0 / 2.2));
    col *= 1.15 * r_brightness; 
    col *= anim_progress;
    gl_FragColor = vec4(col, 1.0);
}
)";

// CINEMA MODE (BVM-D24 Style for Anime/Movies)
static const char *frag_cinema = R"(
#version 100
precision highp float;

varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform float anim_progress;
uniform bool distort_enable;
uniform float r_brightness;

vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.005, 1.005); // Very subtle curve for modern cinema feel
    uv.x *= 1.0 + pow((abs(uv.y) / 6.0), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 5.0), 2.0);
    return (uv / 2.0) + 0.5;
}

// Vibrance: Smart saturation that protects skin tones
vec3 vibrance(vec3 col, float val) {
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    vec3 mask = (col - vec3(lum));
    mask = clamp(mask, 0.0, 1.0);
    float lum_mask = 1.0 - dot(mask, vec3(0.299, 0.587, 0.114));
    lum_mask = 1.0 - pow(lum_mask, 3.0);
    return mix(col, vec3(lum), -val * lum_mask);
}

void main() {
    vec2 uv = uvpos;
    if (distort_enable) uv = curve(uv);

    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // 1. Soft Source: Use standard texture2D for natural blurring of video artifacts
    vec3 col = texture2D(smp, uv).rgb;
    
    // 2. Linearize
    col = pow(col, vec3(2.2));

    // 3. Adaptive Scanlines (Beam Bloom)
    // Dark areas get scanlines, Bright areas (explosions/lights) get smooth
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    float scan_phase = sin(uv.y * resolution.y * 3.14159); // Full resolution scanlines
    float scan_strength = 0.3 * (1.0 - lum); // Fade out scanlines in bright areas
    col *= 1.0 - (scan_strength * (0.5 - 0.5 * scan_phase));

    // 4. Aperture Grille (Trinitron - Vertical Lines only)
    // High TVL (Television Lines) count for HD content
    float grill = sin(uv.x * resolution.x * 3.14159 * 1.5);
    col *= 1.0 - (0.15 * (0.5 - 0.5 * grill));

    // 5. Vibrance Pass (Anime/Movie Color Boost)
    col = pow(col, vec3(1.0 / 2.2)); // Back to Gamma for color ops
    col = vibrance(col, 0.15); // Add 15% vibrance
 
    // 6. Final Brightness
    col *= r_brightness; 

    col *= anim_progress;
    gl_FragColor = vec4(col, 1.0);
}
)";

class wayfire_crt_screen : public wf::per_output_plugin_instance_t {
  wf::post_hook_t hook;
  wf::activator_callback toggle_cb;
  wf::activator_callback cycle_cb;

  // Generic Options
  wf::option_wrapper_t<bool> opt_enable{"crt-effect/enabled"};
  wf::option_wrapper_t<std::string> opt_mode{"crt-effect/mode"};
  wf::option_wrapper_t<wf::activatorbinding_t> toggle_key{"crt-effect/toggle"};
  wf::option_wrapper_t<wf::activatorbinding_t> cycle_key{
      "crt-effect/cycle_mode"};
  wf::option_wrapper_t<int> opt_duration{"crt-effect/duration"};

  // Standard Options
  wf::option_wrapper_t<bool> opt_distort{"crt-effect/distortion"};
  wf::option_wrapper_t<bool> opt_scanlines{"crt-effect/scanlines"};
  wf::option_wrapper_t<bool> opt_vignette{"crt-effect/vignette"};
  wf::option_wrapper_t<bool> opt_aberration{"crt-effect/aberration"};

  // Royale Options
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

  OpenGL::program_t program_discrete;
  OpenGL::program_t program_retro;
  OpenGL::program_t program_royale;
  OpenGL::program_t program_perfect;
  OpenGL::program_t program_cinema; // New Cinema Mode

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
      std::string current = opt_mode;
      std::string next = "discrete";

      if (current == "discrete")
        next = "retro";
      else if (current == "retro")
        next = "royale";
      else if (current == "royale")
        next = "perfect";
      else if (current == "perfect")
        next = "cinema"; // Cycle to cinema
      else if (current == "cinema")
        next = "discrete";

      auto section = wf::get_core().config->get_section("crt-effect");
      auto option = section->get_option("mode");
      option->set_value_str(next);

      output->render->damage_whole();
      return true;
    };

    wf::gles::run_in_context([&] {
      program_discrete.set_simple(
          OpenGL::compile_program(vertex_shader, frag_discrete));
      program_retro.set_simple(
          OpenGL::compile_program(vertex_shader, frag_retro));
      program_royale.set_simple(
          OpenGL::compile_program(vertex_shader, frag_royale));
      program_perfect.set_simple(
          OpenGL::compile_program(vertex_shader, frag_perfect));
      program_cinema.set_simple(
          OpenGL::compile_program(vertex_shader, frag_cinema));
    });

    output->add_activator(toggle_key, &toggle_cb);
    output->add_activator(cycle_key, &cycle_cb);

    if (opt_enable) {
      state = ACTIVE;
      progression = 1.0f;
      output->render->add_post(&hook);
      output->can_activate_plugin(&grab_interface);
    }
  }

  void render(wf::auxilliary_buffer_t &source,
              const wf::render_buffer_t &destination) {
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
      } else {
        output->render->damage_whole();
      }
    } else if (state == FADE_OUT) {
      progression -= dt / duration;
      if (progression <= 0.0f) {
        progression = 0.0f;
        state = INACTIVE;
        output->render->rem_post(&hook);
        return;
      } else {
        output->render->damage_whole();
      }
    }

    static const float vertexData[] = {-1.0f, -1.0f, 1.0f,  -1.0f,
                                       1.0f,  1.0f,  -1.0f, 1.0f};
    static const float coordData[] = {0.0f, 0.0f, 1.0f, 0.0f,
                                      1.0f, 1.0f, 0.0f, 1.0f};

    std::string mode = opt_mode;
    if (mode == "retro")
      output->render->damage_whole();

    wf::gles::run_in_context([&] {
      wf::gles::bind_render_buffer(destination);

      OpenGL::program_t *prog_ptr = &program_discrete;
      if (mode == "retro")
        prog_ptr = &program_retro;
      else if (mode == "royale")
        prog_ptr = &program_royale;
      else if (mode == "perfect")
        prog_ptr = &program_perfect;
      else if (mode == "cinema")
        prog_ptr = &program_cinema;

      auto &prog = *prog_ptr;
      prog.use(wf::TEXTURE_TYPE_RGBA);

      GL_CALL(glBindTexture(GL_TEXTURE_2D,
                            wf::gles_texture_t::from_aux(source).tex_id));
      GL_CALL(glActiveTexture(GL_TEXTURE0));

      prog.attrib_pointer("position", 2, 0, vertexData);
      prog.attrib_pointer("uvPosition", 2, 0, coordData);
      prog.uniform2f("resolution", (float)destination.get_size().width,
                     (float)destination.get_size().height);
      prog.uniform1f("anim_progress", progression);

      if (mode == "retro") {
        float time_sec = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - start_time)
                             .count() /
                         1000.0f;
        prog.uniform1f("time", time_sec);
        prog.uniform1i("distort_enable", opt_distort);
      } else if (mode == "royale") {
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
        prog.uniform1i("distort_enable", opt_distort);
        prog.uniform2f("r_convergence_x", (float)r_conv_x_r, (float)r_conv_x_b);
        prog.uniform2f("r_convergence_y", (float)r_conv_y_r, (float)r_conv_y_b);
      } else if (mode == "perfect") {
        prog.uniform1i("distort_enable", opt_distort);
        prog.uniform1f("r_brightness", (float)r_bright);
      } else if (mode == "cinema") {
        // Cinema mode uses simplified uniforms
        prog.uniform1i("distort_enable", opt_distort);
        prog.uniform1f("r_brightness", (float)r_bright);
      } else {
        prog.uniform1i("distort_enable", opt_distort);
        prog.uniform1i("scanlines_enable", opt_scanlines);
        prog.uniform1i("vignette_enable", opt_vignette);
        prog.uniform1i("aberration_enable", opt_aberration);
      }

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
      program_discrete.free_resources();
      program_retro.free_resources();
      program_royale.free_resources();
      program_perfect.free_resources();
      program_cinema.free_resources();
    });
    output->rem_binding(&toggle_cb);
    output->rem_binding(&cycle_cb);
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_crt_screen>);
