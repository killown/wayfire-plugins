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
#include <wayfire/toplevel-view.hpp>
#include <wayfire/util/duration.hpp>
#include <wayfire/view-helpers.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/view.hpp>
#include <wayfire/window-manager.hpp>
#include <wayfire/workspace-set.hpp>

namespace wf {
namespace hide_view {
class hide_view_data : public wf::custom_data_t {};
class wayfire_hide_view : public wf::plugin_interface_t {
  wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> ipc_repo;

  std::vector<wayfire_view> hidden_views;
  std::vector<pid_t> hidden_pids;
  wf::wl_idle_call idle_refocus;

  /* Borrowed from sway */
  pid_t get_parent_pid(pid_t child) {
    pid_t parent = -1;
    char file_name[100];
    char *buffer = NULL;
    const char *sep = " ";
    FILE *stat = NULL;
    size_t buf_size = 0;

    snprintf(file_name, sizeof(file_name), "/proc/%d/stat", child);

    if ((stat = fopen(file_name, "r"))) {
      if (getline(&buffer, &buf_size, stat) != -1) {
        strtok(buffer, sep);             // pid
        strtok(NULL, sep);               // executable name
        strtok(NULL, sep);               // state
        char *token = strtok(NULL, sep); // parent pid
        parent = strtol(token, NULL, 10);
      }

      free(buffer);
      fclose(stat);
    }

    if (parent) {
      return (parent == child) ? -1 : parent;
    }

    return -1;
  }

public:
  void init() override {
    ipc_repo->register_method("hide-view/run-n-hide", ipc_run_and_hide);
    ipc_repo->register_method("hide-view/hide", ipc_view_hide);
    ipc_repo->register_method("hide-view/unhide", ipc_view_unhide);
  }

  /* soreau code for run and hide */
  wf::ipc::method_callback ipc_run_and_hide =
      [=](nlohmann::json data) -> nlohmann::json {
    WFJSON_EXPECT_FIELD(data, "app", string);

    hidden_pids.push_back(wf::get_core().run(data["app"]));
    wf::get_core().connect(&on_view_mapped);

    return wf::ipc::json_ok();
  };

  wayfire_view find_view_by_id(uint id) {
    for (auto &v : hidden_views) {
      if (v->get_id() == id) {
        return v;
      }
    }
    return nullptr;
  }

  wf::signal::connection_t<wf::view_mapped_signal> on_view_mapped =
      [=](wf::view_mapped_signal *ev) {
        auto view = ev->view;
        if (!view) {
          return;
        }
        bool found = false;
        pid_t view_pid;

        wlr_xwayland_surface *xwayland_surface = NULL;
        if (view->get_wlr_surface()) {
          xwayland_surface = wlr_xwayland_surface_try_from_wlr_surface(
              view->get_wlr_surface());
        }
        if (xwayland_surface) {
          view_pid = xwayland_surface->pid;
        } else {
          wl_client_get_credentials(view->get_client(), &view_pid, 0, 0);
        }
        do {
          for (auto p : hidden_pids) {
            if (p == view_pid) {
              found = true;
              break;
            }
          }
          if (found == true) {
            break;
          }
        } while ((view_pid = get_parent_pid(view_pid)) != -1);
        if (view_pid != -1 && !view->get_data<hide_view_data>()) {
          LOGI("Hiding view with ID: ", view->get_id());
          wf::scene::set_node_enabled(view->get_root_node(), false);
          wf::scene::set_node_enabled(view->get_root_node(), false);
          hide_view_data hv_data;
          view->store_data(std::make_unique<hide_view_data>(hv_data));
          view->role = wf::VIEW_ROLE_UNMANAGED;
          hidden_views.push_back(view);
          auto it = std::find(hidden_pids.begin(), hidden_pids.end(), view_pid);
          if (it != hidden_pids.end()) {
            hidden_pids.erase(it);
          }
          auto output = view->get_output();
          if (auto toplevel = toplevel_cast(view)) {
            output->wset()->remove_view(toplevel);
          }

          output = view->get_output();
          view->set_output(nullptr);
          idle_refocus.run_once([=]() { wf::get_core().seat->refocus(); });
          if (hidden_pids.empty()) {
            on_view_mapped.disconnect();
          }
        }
      };

  wf::ipc::method_callback ipc_view_hide =
      [=](nlohmann::json data) -> nlohmann::json {
    WFJSON_EXPECT_FIELD(data, "view-id", number_unsigned);

    wayfire_toplevel_view view =
        toplevel_cast(wf::ipc::find_view_by_id(data["view-id"]));

    if (view && view->role == wf::VIEW_ROLE_TOPLEVEL) {
      hide_view_data hv_data;
      view->store_data(std::make_unique<hide_view_data>(hv_data));
      wf::scene::set_node_enabled(view->get_root_node(), false);
      view->role = wf::VIEW_ROLE_DESKTOP_ENVIRONMENT;
      auto output = view->get_output();
      if (auto toplevel = toplevel_cast(view)) {
        auto old_wset = output->wset();
        output->wset()->remove_view(toplevel);
        auto target_wset = output->wset();
        wf::emit_view_pre_moved_to_wset_pre(view, old_wset, target_wset);
      }

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

    wayfire_toplevel_view view =
        toplevel_cast(wf::ipc::find_view_by_id(data["view-id"]));
    if (view && (view->role == wf::VIEW_ROLE_DESKTOP_ENVIRONMENT ||
                 view->role == wf::VIEW_ROLE_UNMANAGED)) {
      auto new_output = wf::get_core().seat->get_active_output();
      if (auto toplevel = toplevel_cast(view)) {
        wf::scene::set_node_enabled(toplevel->get_root_node(), true);
        wf::scene::set_node_enabled(toplevel->get_root_node(), true);
        toplevel->role = wf::VIEW_ROLE_TOPLEVEL;
        new_output->wset()->add_view(toplevel);
        toplevel->set_output(new_output);
      }
      return wf::ipc::json_ok();
    } else if (!view) {
      return wf::ipc::json_error("Failed to unhide the view.");
    } else {
      return wf::ipc::json_ok();
    }
  };

  void fini() override {
    for (auto &view : hidden_views) {
      if (view->get_data<hide_view_data>()) {
        wf::scene::set_node_enabled(view->get_root_node(), true);
        wf::scene::set_node_enabled(view->get_root_node(), true);
        view->release_data<hide_view_data>();
        wf::view_mapped_signal map_signal;
        map_signal.view = view;
        wf::get_core().emit(&map_signal);
        wf::get_core().seat->focus_view(view);
        wf::view_bring_to_front(view);
      }
    }
    ipc_repo->unregister_method("hide-view/hide");
    ipc_repo->unregister_method("hide-view/unhide");
    ipc_repo->unregister_method("hide-view/run-n-hide");
    on_view_mapped.disconnect();
  }
};
} // namespace hide_view
} // namespace wf

DECLARE_WAYFIRE_PLUGIN(wf::hide_view::wayfire_hide_view);
