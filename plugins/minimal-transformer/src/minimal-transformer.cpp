#include <wayfire/plugin.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/view-transform.hpp>

class minimal_transformer : public wf::scene::view_2d_transformer_t {
public:
  minimal_transformer(wayfire_view view)
      : wf::scene::view_2d_transformer_t(view) {
    // Set initial transform values
    scale_x = 0.5;
    scale_y = 0.5;
    alpha = 0.8;
  }
};

class minimal_plugin : public wf::plugin_interface_t {
  wf::signal::connection_t<wf::view_mapped_signal> on_map;

  void init() override {
    on_map = [this](auto ev) {
      auto node = ev->view->get_transformed_node();
      if (node && !node->get_transformer("minimal")) {
        LOGI("Adding transformer to new view");
        node->add_transformer(std::make_shared<minimal_transformer>(ev->view),
                              wf::TRANSFORMER_2D, "minimal");
      }
    };

    wf::get_core().connect(&on_map);
  }

  void fini() override {}
};

DECLARE_WAYFIRE_PLUGIN(minimal_plugin);
