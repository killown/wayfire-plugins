# Making Wayfire Talk Pure Vulkan

Alright, let's get into the weeds. If you’re here, you probably realized that the standard Wayfire plugin API is great for GLES but leaves you hanging when you want to use the experimental Vulkan backend. To fix that, we have to stop asking the compositor for permission and start borrowing its power directly. This is a breakdown of how we built a "pure" Vulkan plugin.

## The Architectural Foundation

Normally, a plugin is a guest in Wayfire’s house. You use Wayfire’s functions, and Wayfire handles the hardware. But in our case, we’re more like a contractor who brings their own tools but uses the house’s electricity. We don't create a new Vulkan Instance (the connection to the driver) or a new Device (the connection to the card). We use the ones Wayfire already has open. This keeps everything in one "context" so the GPU doesn't have to switch gears constantly.

### The Borrowed Handles

We use wlr_vk_renderer_get_device and wlr_vk_renderer_get_physical_device. These are the golden keys. They let us grab the specific handles to your AMD or NVIDIA card that the compositor is currently using. By using the same Queue Family, we can submit our own custom command buffers on the same lane that the desktop is using for rendering.

## The Synchronization Nightmare

Vulkan is asynchronous. When you tell it to "Draw a rectangle," the function returns instantly on the CPU, but the GPU might not actually do the work for another 2 or 3 milliseconds. If your CPU code tries to change the rectangle's color before the GPU is done, you get flickering or crashes.

### The Fence Strategy

We solve this with a **Fence**. Think of the Fence as a physical flag.

- At the start of the frame, the CPU checks if the flag is up (Signaled).
- If the flag is down, the CPU waits right there (`vkWaitForFences`).
- Once the GPU finishes the _previous_ frame, it kicks the flag up.
- The CPU sees the flag, resets it, and starts writing new instructions.

This creates a perfect "One-In, One-Out" loop. We created the fence with the `SIGNALED` bit at the start so the first frame doesn't get stuck waiting for a "previous frame" that never happened.

## Memory Management (The Hard Way)

In OpenGL, memory is "hidden." In Vulkan, we have to allocate every byte manually. We created a `scratch_image` for our rectangle and a `dump_buffer` for our debug screenshot. But just creating the objects isn't enough, you have to find a **Memory Heap** that fits.

We use `find_memory_type` to ask the GPU: "Hey, I need 1MB of memory that is 'Host Visible' (so the CPU can read it) and 'Coherent' (so the data is synced instantly)." The GPU gives us back an index, and we use `vkAllocateMemory` to carve out that specific chunk of VRAM. If we forget to free this in our destructor, that VRAM is gone until you restart your computer.

## Layout Transitions and Barriers

This is where things get weird. GPU memory is stored in different patterns depending on what you're doing. A "Transfer" pattern is optimized for copying, while a "Color Attachment" pattern is optimized for shaders. You cannot copy into an image that is in a "Shader Read" state.

**The Barrier Rule:** Every time we touch the screen buffer, we must call a `transition_layout`. We tell the GPU: "Transition the screen from what the compositor was doing to a Transfer Source, do our copy, then transition it back." If you miss the "back" part, Wayfire will try to render the next frame with a broken layout and the whole screen will turn into a strobe light.

## The Hook Flow

Our `render_hook` is where the magic happens. Every frame, the compositor says: "I'm done drawing the windows, here is the final image." We catch it and do the following:

- **Fence Wait:** Make sure the GPU is ready for us.
- **Recording:** Start a command buffer. We use `ONE_TIME_SUBMIT` because the pointers to the screen images might change every frame.
- **Bypass Copy:** We transition Wayfire's images to "Transfer" mode, blit (copy) the whole screen to the output, and then blit our red rectangle on top.
- **The Hand-off:** We transition everything back to the layouts Wayfire expects so it can finish the frame without realizing we were ever there.
- **Submission:** We throw the command buffer at the GPU and tell it to signal the Fence when it's done.

## Conclusion

This plugin is the "starting point" if you intend to create plugins in pure Vulkan.
