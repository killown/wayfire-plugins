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

// 2D Rotation matrix for noise layering
mat2 rot(float a) {
    float s = sin(a), c = cos(a);
    return mat2(c, -s, s, c);
}

// Optimized hash for smooth smoke
float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// Value Noise
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Multi-layered FBM for "wispy" detail
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    mat2 m = rot(0.37);
    for (int i = 0; i < 5; i++) {
        v += a * noise(p);
        p = m * p * 2.02 + vec2(pc.time * 0.1, 0.0);
        a *= 0.5;
    }
    return v;
}

void main() {
    vec2 q = uv - 0.5;
    q.x *= pc.aspect_ratio;
    float dist = length(q);

    // GENTLE MOVEMENT FADE
    // A very subtle dimming only when the window is moving.
    float speed = length(pc.velocity);
    float fade = 1.0 - clamp(speed * 1.5, 0.0, 0.25);

    // TEXTURE SAMPLE
    // Pure background sampling
    vec4 scene = texture(tex, uv);
    vec3 col = scene.rgb * fade;

    // THE SMOKE (Wisps)
    // We use domain warping to make the smoke feel fluid
    vec2 smoke_uv = q * 2.5; // Higher number = more/smaller wisps
    smoke_uv.x -= pc.time * 0.2; // Horizontal movement

    // Create "warped" noise for smoke tendrils
    float n1 = fbm(smoke_uv);
    float n2 = fbm(smoke_uv + n1 + vec2(pc.time * 0.05, pc.time * 0.1));

    // Final smoke density (contrast adjusted for "pleasant" look)
    float density = smoothstep(0.3, 0.7, n2);

    // Smoke Mask: Ensure it doesn't just look like a giant circle
    // This allows smoke to pass through the middle but keeps edges soft
    float mask = smoothstep(0.7, 0.2, dist);
    density *= mask * 0.35; // Cap at 35% opacity

    // COLOR COMPOSITION
    // Use a soft, airy color for the wisps
    vec3 smoke_rgb = vec3(0.9, 0.92, 0.98);

    // Blend smoke using screen-style additive logic
    // col = scene_color * (1.0 - density) + smoke_color * density
    col = mix(col, col + smoke_rgb * 0.2, density);

    // Final output
    outColor = vec4(col, scene.a);
}
