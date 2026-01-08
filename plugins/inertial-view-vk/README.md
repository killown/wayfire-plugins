# Inertial View VK

I built this because views in wayfire felt too static and there was no animations for the vulkan renderer. 

The rendering is handled by a custom scene graph node so it doesn't get in the way of the rest of the compositor, keeping things smooth even when the views are bouncing.

# Easy way to install 
wfctl install plugin https://github.com/killown/wayfire-plugins inertial-view-vk

# Deps
#### Arch Linux
sudo pacman -S shaderc vulkan-devel meson ninja

#### Fedora
sudo dnf install shaderc-devel vulkan-loader-devel meson ninja-build

#### Ubuntu / Debian
sudo apt install libshaderc-dev libvulkan-dev meson ninja-build

## Current State
This is still a work in progress. I'm currently fine-tuning the math in the render override to make sure the effects looks right and doesn't clip at the edges of the view. 



https://github.com/user-attachments/assets/e4547e2b-c248-4158-9159-e13638ae09e8


