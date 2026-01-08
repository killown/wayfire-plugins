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

// High-frequency hash for noise generation
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Cubic interpolated noise for smooth fluid transitions
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
 
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
 
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// 6-Octave fBM for detailed flame structure
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8); // Rotate octaves to prevent grid patterns
    for (int i = 0; i < 6; i++) {
        v += a * noise(p);
        p = rot * p * 2.1;
        a *= 0.5;
    }
    return v;
}

void main() {
    vec2 center = vec2(0.5, 0.5);
    vec2 q = uv - center;
    q.x *= pc.aspect_ratio;
 
    float r = length(q);
    float radius = 0.25; // Controlled fire size in center

    // Standard view lookup
    vec4 sceneColor = texture(tex, uv);

    if (r < radius) {
        // Create Domain Warping for fluid "licking" effect
        // We warp the coordinates of the noise with another noise function
        vec2 warp;
        warp.x = fbm(q * 2.0 + pc.time * 0.4);
        warp.y = fbm(q * 2.0 - pc.time * 0.6);
 
        // Animate coordinates (Upward drift + rotational swirl)
        float swirl_speed = pc.time * 1.5;
        mat2 swirl = mat2(cos(swirl_speed), -sin(swirl_speed),
                          sin(swirl_speed), cos(swirl_speed));
        vec2 fire_uv = (swirl * q) * 4.0;
        fire_uv -= vec2(0.0, pc.time * 2.5); // Fast upward "heat" rise

        // Compute the flame intensity
        float n = fbm(fire_uv + warp);
 
        // Intensity falloff: sharper at the eye, feathered at the edge
        float falloff = pow(1.0 - (r / radius), 2.5);
        float flame = n * falloff * 3.5;

        // Color Grading (Black Body Radiation approximation)
        vec3 col = vec3(0.0);
        // Base Red Glow
        col = mix(vec3(0.0), vec3(1.0, 0.1, 0.0), smoothstep(0.0, 0.7, flame));
        // Orange core
        col = mix(col, vec3(1.0, 0.4, 0.0), smoothstep(0.7, 1.3, flame));
        // Yellow hot-spots
        col = mix(col, vec3(1.0, 0.9, 0.2), smoothstep(1.3, 1.9, flame));
        // White-hot center
        col = mix(col, vec3(1.0, 1.0, 1.0), smoothstep(1.9, 2.5, flame));

        // Blending: Screen/Additive mix to preserve transparency
        // This ensures the fire looks like it's emitting light over the window
        outColor = vec4(sceneColor.rgb + col * falloff, sceneColor.a);
    }
    else {
        outColor = sceneColor;
    }
}
