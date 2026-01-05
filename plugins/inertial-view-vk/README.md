# Inertial View VK

I built this because views in wayfire felt too static and there was no animations for the vulkan renderer. 

The rendering is handled by a custom scene graph node so it doesn't get in the way of the rest of the compositor, keeping things smooth even when the views are bouncing.

# Easy way to install 
wfctl install plugin https://github.com/killown/wayfire-plugins inertial-view-vk

## Current State
This is still a work in progress. I'm currently fine-tuning the math in the render override to make sure the effects looks right and doesn't clip at the edges of the view. 



https://github.com/user-attachments/assets/cd8647c7-c9c6-4ad3-9d2a-f83cc2abbc23

