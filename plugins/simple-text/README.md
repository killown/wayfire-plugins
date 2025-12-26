# Wayfire Simple-Text Plugin Documentation

## Installation

### Dependencies

Ensure the following development headers are present on your system:

- `wayfire` & `wlroots-0.19`
- `cairo` & `pango`
- `glm` (for matrix transformations)

### Build Pipeline

We utilize the Meson build system to ensure deterministic builds.

    # Initialize build directory
    meson setup build --prefix=/usr --buildtype=release

    # Compile and Install
    ninja -C build
    sudo ninja -C build install

### Configuration

Enable the plugin in your `wayfire.ini`:

    [core]
    plugins = ..., simple-text

    [simple-text]

    # Color format: #RRGGBBAA
    color = #EBDBB2FF

## IPC API Reference

### Method: `simple-text/update-display`

Triggers or updates a notification on the currently focused output.

| Parameter | Type    | Default | Description                                     |
| --------- | ------- | ------- | ----------------------------------------------- |
| text      | string  | ""      | Message to display. Supports UTF-8              |
| image     | string  | ""      | Absolute path to a PNG/JPG icon                 |
| font_size | integer | 32      | Pixel height of the text renderer               |
| x         | integer | 100     | Screen X coordinate                             |
| y         | integer | 100     | Screen Y coordinate                             |
| timeout   | integer | 0       | Milliseconds to persist after activity detected |

## Python IPC Integration (Python 3.13+)

    from typing import Any
    from wayfire import WayfireSocket
    sock = WayfireSocket()

    def update_osd(
        self,
        text: str = "",
        image: str = "",
        font_size: int = 32,
        x: int = 100,
        y: int = 100,
        timeout: int = 0,
    ) -> dict[str, Any]:
        """Triggers a notification overlay with deferred activity-based timeout.

        The notification renders immediately and persists until seat activity is
        detected. Once the user interacts with the system, the timeout countdown
        begins before the OSD is dismissed.

        Args:
            text: The message string to display in the overlay.
            image: Absolute filesystem path to an icon (PNG/JPG).
            font_size: Pixel height of the rendered text.
            x: Horizontal screen coordinate for the overlay origin.
            y: Vertical screen coordinate for the overlay origin.
            timeout: Milliseconds to persist after user activity is detected.

        Returns:
            A dictionary containing the IPC response from the compositor.
        """
        payload = {
            "method": "simple-text/update-display",
            "data": {
                "text": text,
                "image": image,
                "font_size": font_size,
                "x": x,
                "y": y,
                "timeout": timeout,
            },
        }
        return sock.send_json(payload)
