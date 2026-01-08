#version 450

layout(push_constant) uniform PushConstants {
    vec2 anchor_pos;
    vec2 velocity;     // Used to tilt the flames
    float spring_k;
    float time;        // Drives the flickering
    float aspect_ratio;
    float margin;
} pc;

layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < 5; i++) {
        v += a * noise(p);
        p = rot * p * 2.0;
        a *= 0.5;
    }
    return v;
}

void main() {
    vec2 center = vec2(0.5, 0.5);
    vec2 q = uv - center;
    q.x *= pc.aspect_ratio;

    // --- ENGINE DYNAMICS ---
    // Calculate how much the flame should tilt based on window velocity
    // We negate the velocity because the flame trails BEHIND the movement
    vec2 tilt = pc.velocity * 0.15; 
    vec2 warped_q = q + tilt * length(q); 
 
    float r = length(warped_q);
    float radius = 0.22;

    vec4 sceneColor = texture(tex, uv);

    if (r < radius) {
        // Higher velocity stretches the flame
        float speed = length(pc.velocity);
        float stretch = 1.0 + speed * 0.1;

        // Animate flame with upward drift and velocity influence
        vec2 fire_uv = warped_q * vec2(3.0, 3.0 / stretch);
        fire_uv -= vec2(0.0, pc.time * 3.0); 
 
        // Add turbulence based on movement
        float n = fbm(fire_uv + pc.velocity * 0.5);
 
        // Intensity falloff
        float strength = pow(1.0 - (r / radius), 2.0);
        float flame = n * strength * (3.0 + speed * 0.5);

        // Heat Gradient
        vec3 col = vec3(0.0);
        col = mix(vec3(0.0), vec3(0.2, 0.5, 1.0), smoothstep(0.0, 0.5, flame)); // Blue base (Engine)
        col = mix(col, vec3(0.8, 0.2, 1.0), smoothstep(0.5, 1.2, flame));       // Purple core
        col = mix(col, vec3(1.0, 0.9, 1.0), smoothstep(1.2, 2.0, flame));       // White center

        // Additive blending
        outColor = vec4(sceneColor.rgb + col * strength, sceneColor.a);
    } else {
        outColor = sceneColor;
    }
}
