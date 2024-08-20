from wayfire import WayfireSocket as OriginalWayfireSocket
from wayfire.core.template import get_msg_template

class WayfireSocket(OriginalWayfireSocket):
    def run_n_hide(self, app):
        message = get_msg_template("hide-view/run_n_hide")
        message["data"]["app"] = app
        return self.send_json(message) 

    def hide_view(self, view_id):
        message = get_msg_template("hide-view/hide")
        message["data"]["view-id"] = view_id
        return self.send_json(message)

    def unhide_view(self, view_id):
        message = get_msg_template("hide-view/unhide")
        message["data"]["view-id"] = view_id
        return self.send_json(message)

if __name__ == "__main__":
    import sys

    if len(sys.argv) != 3:
        print("Usage: python script.py <hide-view|unhide-view> <view_id>")
        sys.exit(1)

    command = sys.argv[1]
    view_id = int(sys.argv[2])

    sock = WayfireSocket()

    if command == "hide-view":
        response = sock.hide_view(view_id)
    elif command == "unhide-view":
        response = sock.unhide_view(view_id)
    else:
        print(f"Unknown command: {command}")
        sys.exit(1)

    print(f"Response: {response}")

