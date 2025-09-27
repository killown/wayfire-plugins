#pragma once

#include <cstdint>
#include <dlfcn.h>
#include <limits.h>
#include <optional>
#include <string>
#include <unistd.h>

#include <wayfire/config.h>
#include <wayfire/core.hpp>
#include <wayfire/plugin.hpp>
#include <wayfire/plugins/common/shared-core-data.hpp>
#include <wayfire/plugins/ipc/ipc-helpers.hpp>
#include <wayfire/plugins/ipc/ipc-method-repository.hpp>

namespace wf {
template <typename Target, typename Source> Target union_cast(Source value) {
  static_assert(sizeof(Target) == sizeof(Source),
                "union_cast: source and target must have the same size");
  union {
    Source source;
    Target target;
  } cast_union;
  cast_union.source = value;
  return cast_union.target;
}
using wayfire_plugin_version_func = std::uint32_t (*)();
static std::string get_stdout_target_path() {
  const char *fd_path = "/proc/self/fd/1";
  char buffer[PATH_MAX];
  ssize_t len;
  if (access(fd_path, F_OK) == -1) {
    return "NotSupportedOrMissing";
  }
  len = readlink(fd_path, buffer, sizeof(buffer) - 1);
  if (len != -1) {
    buffer[len] = '\0';
    std::string target(buffer);
    if (target.empty() || (target.rfind("pipe", 0) == 0) ||
        (target.rfind("socket", 0) == 0)) {
      return "PipeOrSocket";
    }
    if (target == "/dev/null") {
      return "DevNull";
    }
    if ((target.rfind("/dev/pts", 0) == 0) ||
        (target.rfind("/dev/tty", 0) == 0)) {
      return "Terminal";
    }
    return target;
  }
  return "ReadlinkFailed";
}
class ipc_plugin_version_t : public wf::plugin_interface_t {
public:
  void init() override {
    if (method_repository.get()) {
      method_repository->register_method("wayfire/get-plugin-abi-version",
                                         get_plugin_abi_version);
      method_repository->register_method("wayfire/get-stdout-redirect-path",
                                         get_stdout_redirect_path);
    }
  }
  void fini() override {
    if (method_repository.get()) {
      method_repository->unregister_method("wayfire/get-plugin-abi-version");
      method_repository->unregister_method("wayfire/get-stdout-redirect-path");
    }
  }

private:
  wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> method_repository;
  wf::ipc::method_callback get_stdout_redirect_path =
      [=](const wf::json_t &data) -> wf::json_t {
    std::string path = get_stdout_target_path();
    wf::json_t response = wf::ipc::json_ok();
    response["path"] = path;
    if (path.rfind('/', 0) == 0) {
      if (path == "/dev/null") {
        response["status"] = "discarded";
      } else if ((path.rfind("/dev/pts", 0) == 0) ||
                 (path.rfind("/dev/tty", 0) == 0)) {
        response["status"] = "terminal";
      } else {
        response["status"] = "file_redirected";
      }
    } else if (path == "PipeOrSocket") {
      response["status"] = "piped";
    } else if (path == "Terminal") {
      response["status"] = "terminal";
    } else {
      response["status"] = "error";
    }
    return response;
  };
  wf::ipc::method_callback get_plugin_abi_version =
      [=](const wf::json_t &data) -> wf::json_t {
    auto path_opt = wf::ipc::json_get_optional_string(data, "path");
    if (!path_opt.has_value()) {
      return wf::ipc::json_error("Missing 'path' parameter!");
    }
    std::string path = path_opt.value();
    void *handle = dlopen(path.c_str(), RTLD_LOCAL | RTLD_LAZY);
    if (!handle) {
      return wf::ipc::json_error(std::string("Failed to load plugin: ") +
                                 dlerror());
    }
    auto version_func_ptr = dlsym(handle, "getWayfireVersion");
    if (!version_func_ptr) {
      dlclose(handle);
      return wf::ipc::json_error(
          "Symbol 'getWayfireVersion' not found in plugin!");
    }
    auto version_func =
        wf::union_cast<wayfire_plugin_version_func>(version_func_ptr);
    std::uint32_t plugin_abi_version = version_func();
    dlclose(handle);
    wf::json_t response = wf::json_t();
    response["wayfire_abi_version"] = WAYFIRE_API_ABI_VERSION;
    response["plugin_abi_version"] = plugin_abi_version;
    response["compatible"] = (plugin_abi_version == WAYFIRE_API_ABI_VERSION);
    return response;
  };
};
} // namespace wf
DECLARE_WAYFIRE_PLUGIN(wf::ipc_plugin_version_t);
