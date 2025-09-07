#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/util/log.hpp>

/**
 * damage_logger - Simple plugin to log repaint events
 */

class damage_logger_plugin_t : public wf::per_output_plugin_instance_t {
  wf::effect_hook_t on_render_pass_done;
  bool logging_active = false;

public:
  void init() override {
    on_render_pass_done = [=]() {
      if (logging_active) {
        LOGI("[damage_log] Render pass completed");
      }
    };

    output->render->add_effect(&on_render_pass_done,
                               wf::OUTPUT_EFFECT_PASS_DONE);
    logging_active = true;
  }

  void fini() override {
    logging_active = false;
    output->render->rem_effect(&on_render_pass_done);
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<damage_logger_plugin_t>);
