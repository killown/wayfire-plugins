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

// Multi-octave fBM for the Accretion Disk gas
float fbm(vec2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 6; i++) {
        v += a * noise(p);
        p = rot(0.5) * p * 2.0 + vec2(10.0);
        a *= 0.5;
    }
    return v;
}

void main() {
    vec2 center = vec2(0.5, 0.5);
    vec2 q = uv - center;
    q.x *= pc.aspect_ratio;
    float dist = length(q);
 
    // --- RELATIVISTIC PARAMETERS ---
    float rs = 0.08;         // Schwarzschild Radius (Event Horizon)
    float photon_sphere = rs * 1.5; 
 
    // GRAVITATIONAL LENSING
    // Light paths are curved. We calculate the deflection angle.
    // Near the horizon, light is "sucked" from the rest of the texture.
    float deflection = (2.0 * rs) / (dist - rs + 0.001);
    vec2 warped_uv = uv - normalize(q) * (deflection * 0.05);

    // ACCRETION DISK (Volumetric Gas)
    // We create a rotating disk of superheated gas around the hole.
    float angle = atan(q.y, q.x);
    vec2 disk_uv = vec2(dist * 5.0 - pc.time * 0.5, angle * 2.0);
    float gas = fbm(disk_uv + fbm(disk_uv * 1.5));

    // Mask the disk: It exists between the horizon and a certain distance
    float disk_mask = smoothstep(rs, rs + 0.05, dist) * smoothstep(0.45, 0.3, dist);
    vec3 disk_color = vec3(1.0, 0.4, 0.1) * gas * disk_mask;

    // PHOTON RING (The bright edge)
    float ring = exp(-pow(dist - photon_sphere, 2.0) * 2000.0);
    vec3 ring_color = vec3(1.0, 0.9, 0.8) * ring * 2.0;

    // SAMPLING & FINAL COMPOSITION
    if (dist < rs) {
        // Inside the Event Horizon: Total Darkness
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        vec4 scene = texture(tex, warped_uv);

        // Gravitational Redshift (Scene darkens as it approaches RS)
        float redshift = smoothstep(rs, rs + 0.2, dist);
        vec3 final_rgb = scene.rgb * redshift;

        // Add Accretion Disk and Photon Ring
        final_rgb += disk_color + ring_color;

        // Lens Flare / Bloom at the edges of the disk
        final_rgb += disk_color * pow(gas, 3.0) * 0.5;

        outColor = vec4(final_rgb, scene.a);
    }
}
