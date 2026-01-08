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

// Better hash for temporal stability
float hash12(vec2 p) {
    vec3 p3  = fract(vec3(p.xyx) * .1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

/**
 * @brief High-fidelity rain layer with refraction and velocity-based shear.
 */
vec3 rainLayer(vec2 uv_p, float t, float layer_idx) {
    vec2 p = uv_p;
 
    // Downward movement. Subtracting time from Y.
    // Also adding a 'wind' tilt based on window velocity
    float wind = pc.velocity.x * 0.1;
    p.x += wind * p.y; 
    p.y -= t; 

    // Grid scaling
    vec2 grid_size = vec2(15.0, 1.0); 
    vec2 st = p * grid_size;
    vec2 id = floor(st);
    vec2 f = fract(st) - 0.5;

    float h = hash12(id + layer_idx);
 
    // Vertical jitter for drop variety
    float y_off = fract(h * 123.4);
    f.y += (y_off - 0.5) * 0.8;
 
    // Drop shape: elongated capsule
    // We adjust the X scale based on velocity to simulate 'streaking'
    float streak = 1.0 + length(pc.velocity) * 5.0;
    f.x *= 1.5;
    f.y /= streak;

    float d = length(f);
 
    // Sharper mask to remove 'box' artifacts
    float mask = smoothstep(0.15, 0.05, d);
 
    // Refraction logic: Sample the background with a distortion
    // Offset is calculated using the drop's internal coordinates (normal)
    vec2 refract_offset = f * mask * 0.04;
 
    // Sample channels separately for Chromatic Aberration in the water
    float r = texture(tex, uv_p + refract_offset * 1.1).r;
    float g = texture(tex, uv_p + refract_offset).g;
    float b = texture(tex, uv_p + refract_offset * 0.9).b;
    vec3 color = vec3(r, g, b);
 
    // Specular highlight (the 'glint' of light)
    float light = pow(max(0.0, 1.0 - d * 8.0), 6.0) * mask;
 
    return (color + light * 0.8) * mask;
}

void main() {
    vec2 p = uv;
    p.x *= pc.aspect_ratio;
 
    // Scene pre-process (Cold, moody color grade)
    vec4 scene = texture(tex, uv);
    scene.rgb *= vec3(0.8, 0.82, 0.9); // Tint
    scene.rgb *= 0.9;                  // Exposure

    // Three Layers of Depth (Parallax)
    // Front layer (fastest, largest, most refractive)
    vec3 l1 = rainLayer(p, pc.time * 4.5, 0.123);
    // Mid layer
    vec3 l2 = rainLayer(p * 2.2, pc.time * 3.0, 0.456);
    // Back layer (farthest, smallest)
    vec3 l3 = rainLayer(p * 4.5, pc.time * 1.8, 0.789);

    // Blending
    // We blend front-to-back using a simple additive/mix hybrid
    float m1 = length(l1);
    float m2 = length(l2);
    float m3 = length(l3);
 
    vec3 rain_final = l1;
    rain_final = mix(rain_final, l2, (1.0 - m1));
    rain_final = mix(rain_final, l3, (1.0 - m1 - m2));

    // Fog/Mist (adds mass to the air)
    float mist = sin(uv.y * 100.0 + pc.time) * 0.01;

    // Final Composite
    float total_mask = clamp(m1 + m2 + m3, 0.0, 1.0);
    vec3 result = mix(scene.rgb + mist, rain_final, total_mask);

    // Subtle screen-space reflections (Vertical stretch)
    if (total_mask > 0.0) {
        result += l1 * 0.2; // Extra pop for foreground drops
    }

    outColor = vec4(result, scene.a);
}
