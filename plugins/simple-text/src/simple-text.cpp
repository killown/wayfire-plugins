#include <chrono>
#include <wayfire/core.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/output.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/plugins/common/cairo-util.hpp>
#include <wayfire/plugins/common/shared-core-data.hpp>
#include <wayfire/plugins/ipc/ipc-helpers.hpp>
#include <wayfire/plugins/ipc/ipc-method-repository.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/seat.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/util.hpp>

class wayfire_simple_text : public wf::plugin_interface_t {
  struct output_state_t {
    wf::cairo_text_t text_renderer;
    wf::wl_timer<false> timeout_timer;
    bool is_visible = false;
    bool timer_started = false;
    int pending_timeout = 0;
    wf::point_t pos = {100, 100};
    wf::effect_hook_t render_hook;
    wf::output_t *output;
    std::chrono::steady_clock::time_point last_update;
  };

  std::map<wf::output_t *, std::unique_ptr<output_state_t>> states;
  wf::option_wrapper_t<wf::color_t> opt_color{"simple-text/color"};
  wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> method_repository;

  wf::signal::connection_t<wf::seat_activity_signal> on_activity = [this](
                                                                       void *) {
    auto now = std::chrono::steady_clock::now();
    for (auto &[out, st] : states) {
      if (st->is_visible && !st->timer_started) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - st->last_update)
                           .count();

        if (elapsed > 100) {
          st->timer_started = true;
          if (st->pending_timeout > 0) {
            st->timeout_timer.set_timeout(st->pending_timeout,
                                          [st = st.get()]() {
                                            st->is_visible = false;
                                            st->timer_started = false;
                                            st->output->render->damage_whole();
                                            return false;
                                          });
          }
        }
      }
    }
  };

  void render_output(output_state_t *st) {
    if (!st->is_visible)
      return;

    auto fb = st->output->render->get_target_framebuffer();
    auto size = st->text_renderer.get_size();
    if (size.width <= 0)
      return;

    wf::geometry_t geom = {st->pos.x, st->pos.y, size.width, size.height};

#if WAYFIRE_API_ABI_VERSION_MACRO < 20250519
    OpenGL::render_begin(fb);
    OpenGL::render_transformed_texture(wf::texture_t{st->text_renderer.tex.tex},
                                       geom, fb.get_orthographic_projection(),
                                       glm::vec4(1.0),
                                       OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
    OpenGL::render_end();
#else
    st->output->render->get_current_pass()->add_texture(
        wf::texture_t{st->text_renderer.get_texture()}, fb, geom, fb.geometry);
#endif
  }

  wf::ipc::method_callback update_display_handler =
      [=](const wf::json_t &data) -> wf::json_t {
    std::string text = data.has_member("text") ? (std::string)data["text"] : "";
    int font_size = data.has_member("font_size") ? (int)data["font_size"] : 32;
    int timeout = data.has_member("timeout") ? (int)data["timeout"] : 0;

    auto focused = wf::get_core().seat->get_active_output();
    if (states.count(focused)) {
      auto &st = states[focused];
      st->pos.x = data.has_member("x") ? (int)data["x"] : st->pos.x;
      st->pos.y = data.has_member("y") ? (int)data["y"] : st->pos.y;

      wf::cairo_text_t::params p;
      p.font_size = font_size;
      p.bg_color = {0, 0, 0, 0.6};
      p.text_color = opt_color;
      st->text_renderer.render_text(text, p);

      st->is_visible = !text.empty();
      st->timer_started = false;
      st->pending_timeout = timeout;
      st->last_update = std::chrono::steady_clock::now();

      st->output->render->rem_effect(&st->render_hook);
      st->output->render->add_effect(&st->render_hook,
                                     wf::OUTPUT_EFFECT_OVERLAY);
      st->timeout_timer.disconnect();
      st->output->render->damage_whole();
    }
    return wf::ipc::json_ok();
  };

public:
  void init() override {
    for (auto &out : wf::get_core().output_layout->get_outputs()) {
      auto st = std::make_unique<output_state_t>();
      st->output = out;
      st->render_hook = [this, s = st.get()]() { render_output(s); };
      states[out] = std::move(st);
    }

    wf::get_core().connect(&on_activity);
    if (method_repository.get())
      method_repository->register_method("simple-text/update-display",
                                         update_display_handler);
  }

  void fini() override {
    for (auto &[out, st] : states)
      out->render->rem_effect(&st->render_hook);
    if (method_repository.get())
      method_repository->unregister_method("simple-text/update-display");
  }
};

DECLARE_WAYFIRE_PLUGIN(wayfire_simple_text);
