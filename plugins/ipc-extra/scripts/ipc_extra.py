from wayfire import WayfireSocket
from wayfire.core.template import get_msg_template


def get_stdout_redirect_path(self):
    """
    Queries the Wayfire compositor process to discover where its standard
    output (stdout, file descriptor 1) is currently redirected.

    The response contains:
    - 'path': The raw target path (e.g., '/home/user/log.txt', 'PipeOrSocket').
    - 'status': A classification (e.g., 'file_redirected', 'terminal', 'piped', 'error').

    :returns: The JSON response dictionary from the compositor.
    """
    message = get_msg_template("wayfire/get-stdout-redirect-path")
    return self.send_json(message)


def get_plugin_abi_version(self, path: str):
    """
    Queries the ABI version of a dynamically loaded Wayfire plugin.
    Maps to the 'wayfire/get-plugin-abi-version' IPC method.
    """
    message = get_msg_template("wayfire/get-plugin-abi-version")
    message["data"]["path"] = path
    # NOTE: The print(message) call is preserved exactly as requested.
    print(message)
    return self.send_json(message)


# Inject the new methods directly into the existing WayfireSocket class
setattr(WayfireSocket, "get_stdout_redirect_path", get_stdout_redirect_path)
setattr(WayfireSocket, "get_plugin_abi_version", get_plugin_abi_version)

print(
    "Successfully injected 'get_configuration' and 'get_plugin_abi_version' into WayfireSocket class."
)

sock = WayfireSocket()
print(sock.get_plugin_abi_version("/usr/lib/wayfire/libalpha.so"))
print(sock.get_stdout_redirect_path())
