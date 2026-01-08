#version 450

layout(push_constant) uniform PushConstants {
    vec2 anchor_pos;
    vec2 velocity;
    float spring_k;
    float time;
    float aspect_ratio;
    float margin;
} pc;

layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

void main() {
    // Calculate Blockiness based on speed
    float speed = length(pc.velocity);

    // Smooth transition between high res (512) and blocky (32)
    float min_blocks = 32.0;
    float max_blocks = 512.0;
    float blocks = mix(max_blocks, min_blocks, clamp(speed * 8.0, 0.0, 1.0));
 
    // Quantize UV (The Voxel Snap)
    // Forces pixels within a 'block' to sample the same point
    vec2 block_uv = floor(uv * blocks) / blocks;

    // Sample the texture - Pure window color
    vec4 col = texture(tex, block_uv);
 
    // Grid Outlines / Bevel Logic
    // Using fract to find local coordinates within the block [0, 1]
    vec2 grid = fract(uv * blocks);

    // Corrected Syntax: Subtle dark shadow on the bottom-right edges
    // This defines the 'pieces' visually without using extra white color
    float b_val = smoothstep(0.0, 0.1, grid.x) * smoothstep(0.0, 0.1, grid.y);
    col.rgb *= mix(0.9, 1.0, b_val);

    outColor = vec4(col.rgb, col.a);
}
