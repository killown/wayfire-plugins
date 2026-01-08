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

mat2 rot(float a) {
    float s = sin(a), c = cos(a);
    return mat2(c, -s, s, c);
}

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

/**
 * @brief 7-Octave spectral noise for cold, wispy smoke.
 */
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 7; i++) {
        v += a * noise(p);
        p = rot(0.5) * p * 2.2 + vec2(10.0);
        a *= 0.48;
    }
    return v;
}

void main() {
    vec2 center = vec2(0.5, 0.5);
    vec2 q = (uv - center);
    q.x *= pc.aspect_ratio;
    float dist = length(q);

    vec4 scene = texture(tex, uv);

    // THE FADE (Heavy Vignette)
    // Darker atmosphere consistent with the Abyss.
    float vignette = mix(0.1, 0.65, smoothstep(0.0, 0.85, dist));
    vec3 fadedScene = scene.rgb * vignette;

    // UNIFIED SMOKE LOGIC
    // Base coordinates for both center wisp and top clouds.
    vec2 smoke_uv = q * 1.5;

    // Core Abyssal Smoke (Center)
    // Vertical drift with slight horizontal sway
    float center_fbm = fbm(smoke_uv + vec2(sin(pc.time * 0.5) * 0.1, -pc.time * 0.25));
    float center_mask = smoothstep(0.45, 0.0, dist);
 
    // Passing Clouds (Top)
    // Stretched horizontally and swept by "wind"
    vec2 sky_uv = q * vec2(0.4, 2.0);
    sky_uv.x += pc.time * 0.45;
    float sky_fbm = fbm(sky_uv + vec2(pc.time * 0.05, 0.0));
    float sky_mask = smoothstep(-0.1, 0.5, q.y); // Clouds exist from middle to top

    // COLOR PALETTE (No Reds)
    // Abyss (Bottom/Center) -> Cold Ash (Mid) -> Moonlight (Top)
    vec3 abyssCol = vec3(0.01, 0.015, 0.02); // Pure deep cold
    vec3 ashCol   = vec3(0.18, 0.2, 0.22);   // Neutral grey-blue
    vec3 skyCol   = vec3(0.5, 0.55, 0.7);    // Ghostly moonlight

    // Combine density
    float d_center = center_fbm * center_mask;
    float d_sky    = sky_fbm * sky_mask;
    float total_density = clamp(d_center + d_sky, 0.0, 1.0);

    // Vertical Color Blending
    vec3 smokeRGB = mix(abyssCol, ashCol, smoothstep(-0.5, 0.1, q.y));
    smokeRGB = mix(smokeRGB, skyCol, d_sky);

    // Volume shadowing to give depth to the passing clouds
    smokeRGB *= (total_density * 1.6);

    // FINAL COMPOSITION
    // No artificial glowing balls, just the interaction of smoke and shadow.
    vec3 result = mix(fadedScene, smokeRGB, total_density * 0.85);

    outColor = vec4(result, scene.a);
}
