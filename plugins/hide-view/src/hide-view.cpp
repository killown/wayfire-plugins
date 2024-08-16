/*

MIT License

Copyright (c) 2024 Thiago <systemofdown@gmail.co>

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

#include <wayfire/core.hpp>
#include <wayfire/output.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/plugins/common/shared-core-data.hpp>
#include <wayfire/plugins/ipc/ipc-helpers.hpp>
#include <wayfire/plugins/ipc/ipc-method-repository.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/view-helpers.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/view.hpp>

namespace wf {
namespace hide_view {
class hide_view_data : public wf::custom_data_t {};
class wayfire_hide_view : public wf::plugin_interface_t {
  wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> ipc_repo;

public:
  void init() override {
    ipc_repo->register_method("hide-view/hide", ipc_view_hide);
    ipc_repo->register_method("hide-view/unhide", ipc_view_unhide);
  }

  wf::ipc::method_callback ipc_view_hide =
      [=](nlohmann::json data) -> nlohmann::json {
    WFJSON_EXPECT_FIELD(data, "view-id", number_unsigned);

    auto view = wf::ipc::find_view_by_id(data["view-id"]);
    if (view && view->role == wf::VIEW_ROLE_TOPLEVEL) {
      hide_view_data hv_data;
      view->store_data(std::make_unique<hide_view_data>(hv_data));
      wf::scene::set_node_enabled(view->get_root_node(), false);
      view->role = wf::VIEW_ROLE_DESKTOP_ENVIRONMENT;
      wf::view_unmapped_signal unmap_signal;
      unmap_signal.view = view;
      auto output = view->get_output();
      if (auto toplevel = toplevel_cast(view)) {
        output->wset()->remove_view(toplevel);
      }
      wf::get_core().emit(&unmap_signal);

      return wf::ipc::json_ok();
    } else if (!view) {
      return wf::ipc::json_error("Failed to hide the view.");
    } else {
      return wf::ipc::json_ok();
    }
  };

  wf::ipc::method_callback ipc_view_unhide =
      [=](nlohmann::json data) -> nlohmann::json {
    WFJSON_EXPECT_FIELD(data, "view-id", number_unsigned);

    auto view = wf::ipc::find_view_by_id(data["view-id"]);
    if (view && view->role == wf::VIEW_ROLE_DESKTOP_ENVIRONMENT) {
      wf::scene::set_node_enabled(view->get_root_node(), true);
      view->role = wf::VIEW_ROLE_TOPLEVEL;
      wf::view_mapped_signal map_signal;
      map_signal.view = view;
      auto output = view->get_output();
      if (auto toplevel = toplevel_cast(view)) {
        output->wset()->add_view(toplevel);
      }
      wf::get_core().emit(&map_signal);

      return wf::ipc::json_ok();
    } else if (!view) {
      return wf::ipc::json_error("Failed to unhide the view.");
    } else {
      return wf::ipc::json_ok();
    }
  };

  void fini() override {
    ipc_repo->unregister_method("hide-view/hide");
    ipc_repo->unregister_method("hide-view/unhide");
  }
};
} // namespace hide_view
} // namespace wf

DECLARE_WAYFIRE_PLUGIN(wf::hide_view::wayfire_hide_view);
