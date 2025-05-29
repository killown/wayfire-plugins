#include <wayfire/plugin.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/view-transform.hpp>

class minimal_transformer : public wf::scene::view_2d_transformer_t {
public:
  minimal_transformer(wayfire_view view)
      : wf::scene::view_2d_transformer_t(view) {

    translation_x = 0.0; // X offset in pixels
    translation_y = 0.0; // Y offset in pixels

    // Scale (1.0 = normal size)
    scale_x = 1.0;
    scale_y = 1.0;

    // Opacity (1.0 = fully visible, 0.0 = invisible)
    alpha = 1.0;

    // An angle in radians indicating how much the view should be rotated
    // around its center counter-clockwise.
    angle = 0.0;
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
