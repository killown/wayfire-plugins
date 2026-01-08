#version 450

layout(push_constant) uniform PushConstants {
    vec2 anchor_pos;
    vec2 velocity;     // Influences the momentum of the seconds hand
    float spring_k;
    float time;
    float aspect_ratio;
    float margin;
} pc;

layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

#define PI 3.14159265359

// 2D Rotation matrix
mat2 rot(float a) {
    float s = sin(a), c = cos(a);
    return mat2(c, -s, s, c);
}

// Capsule SDF for the hands
float sdCapsule(vec2 p, vec2 a, vec2 b, float r) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

void main() {
    vec2 center = vec2(0.5, 0.5);
    vec2 q = uv - center;
    q.x *= pc.aspect_ratio;
    float dist = length(q);
 
    vec4 scene = texture(tex, uv);
 
    // --- CLOCK FACE ---
    float radius = 0.25;
    float face = smoothstep(radius, radius - 0.005, dist);
    float rim = smoothstep(radius + 0.01, radius, dist) * smoothstep(radius - 0.01, radius, dist);
 
    // --- CLOCK LOGIC ---
    // Calculate time angles
    float hour_angle = (pc.time / 3600.0) * 2.0 * PI;
    float min_angle = (pc.time / 60.0) * 2.0 * PI;
 
    // SECONDS LOGIC (Reactive to view velocity)
    // We add a 'bob' effect based on velocity.x and velocity.y
    float velocity_influence = (pc.velocity.x - pc.velocity.y) * 0.5;
    float sec_angle = (pc.time * 2.0 * PI) + (velocity_influence * 2.0);

    // --- DRAWING ---
    float d = 1e10;

    // Hour hand (Short, Thick)
    vec2 h_p = rot(hour_angle) * q;
    d = min(d, sdCapsule(h_p, vec2(0.0), vec2(0.0, 0.12), 0.012));

    // Minute hand (Long, Medium)
    vec2 m_p = rot(min_angle) * q;
    d = min(d, sdCapsule(m_p, vec2(0.0), vec2(0.0, 0.2), 0.008));

    // Seconds hand (Thin, Reactive)
    // We add a slight vibration/jitter if the window is moving fast
    float jitter = sin(pc.time * 100.0) * length(pc.velocity) * 0.02;
    vec2 s_p = rot(sec_angle + jitter) * q;
    float seconds_hand = sdCapsule(s_p, vec2(0.0, -0.05), vec2(0.0, 0.22), 0.003);
 
    // Ticks (Hour marks)
    float ticks = 1e10;
    for(int i=0; i<12; i++) {
        float a = float(i) * (PI / 6.0);
        vec2 tp = rot(a) * q;
        ticks = min(ticks, sdCapsule(tp, vec2(0.0, radius - 0.03), vec2(0.0, radius - 0.01), 0.005));
    }

    // --- COLORING ---
    vec3 clockCol = vec3(0.95); // White face
    vec3 rimCol = vec3(0.2);    // Dark rim
    vec3 handCol = vec3(0.1);   // Black hands
    vec3 secCol = vec3(0.8, 0.1, 0.1); // Red seconds hand

    vec3 finalRGB = scene.rgb;

    if (face > 0.0) {
        finalRGB = mix(finalRGB, clockCol, face);
        finalRGB = mix(finalRGB, vec3(0.5), ticks < 0.0 ? 1.0 : 0.0);
        finalRGB = mix(finalRGB, handCol, d < 0.0 ? 1.0 : 0.0);
        finalRGB = mix(finalRGB, secCol, seconds_hand < 0.0 ? 1.0 : 0.0);
    }
 
    finalRGB = mix(finalRGB, rimCol, rim);

    // Add a central pin
    float pin = smoothstep(0.01, 0.008, dist);
    finalRGB = mix(finalRGB, handCol, pin);

    outColor = vec4(finalRGB, scene.a);
}
