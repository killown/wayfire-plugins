#version 450

// Vulkan requires explicit locations for stage inputs and outputs
layout(location = 0) in vec2 uvpos;
layout(location = 0) out vec4 outColor;

// Binding 0 is our screen texture sampler
layout(binding = 0) uniform sampler2D smp;

/**
 * Push Constant Block
 * MUST match the C++ struct alignment exactly.
 * Note: 'bool' replaced with 'int' to ensure 4-byte alignment parity with C++.
 */
layout(push_constant) uniform PushConstants {
    vec2  resolution;
    float time;
    float anim_progress;
    int   r_mask_type;
    float r_beam_sigma;
    float r_border_size;
    float r_scanline_weight;
    float r_brightness;
    vec2  r_convergence_x;
    vec2  r_convergence_y;
    int   distort_enable; 
} pcs;

/**
 * Simulates CRT tube curvature using a pincushion distortion math.
 */
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.0 + pcs.r_border_size);
    uv.x *= 1.0 + pow((abs(uv.y) / 4.5), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 3.5), 2.0);
    return (uv / 2.0) + 0.5;
}

void main() {
    vec2 uv = uvpos;
    
    // Check int parity for distortion flag
    if (pcs.distort_enable != 0) {
        uv = curve(uv);
    }

    // Border check to prevent texture wrapping/clamping artifacts on the 'tube' edges
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        outColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Convergence logic (Chromatic Aberration)
    vec2 uv_r = uv + vec2(pcs.r_convergence_x.x, pcs.r_convergence_y.x);
    vec2 uv_g = uv;
    vec2 uv_b = uv + vec2(pcs.r_convergence_x.y, pcs.r_convergence_y.y);

    vec3 col;
    col.r = texture(smp, uv_r).r;
    col.g = texture(smp, uv_g).g;
    col.b = texture(smp, uv_b).b;

    // Linear space conversion for mathematically correct lighting/blending
    col = pow(col, vec3(2.2));

    // CRT Beam / Scanline Logic
    // Calculates a Gaussian distribution based on the vertical fragment position
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    float scan_pos = fract(uv.y * pcs.resolution.y * 0.5) - 0.5;
    float sigma = pcs.r_beam_sigma + (lum * 0.1);
    float beam = exp(-(scan_pos * scan_pos) / (2.0 * sigma * sigma));
    col *= 1.0 - (pcs.r_scanline_weight * (1.0 - beam));

    // Shadow Mask Logic (Aperture Grille / Slot Mask simulation)
    float px = uv.x * pcs.resolution.x;
    float py = uv.y * pcs.resolution.y;
    float mask = 1.0;

    if (pcs.r_mask_type == 0) {
        // Simple Trinitron-style Aperture Grille
        mask = 0.85 + 0.15 * sin(px * 3.14159 * 1.5);
    } else if (pcs.r_mask_type == 1) {
        // Slot Mask simulation with staggered vertical bars
        float odd = mod(floor(py / 4.0), 2.0);
        float slot = sin((px + odd * 1.5) * 3.14159 * 1.5);
        float vert = sin(py * 3.14159);
        mask = 0.8 + 0.2 * (slot * vert);
    } else {
        // Shadow Mask (Dot Triad) simulation
        mask = 0.8 + 0.2 * (sin(px * 3.0) * sin(py * 3.0));
    }

    col *= mask;

    // Re-gamma (2.2) and Global Adjustments
    col = pow(col, vec3(1.0 / 2.2));
    col *= pcs.r_brightness;
    
    // Critical: anim_progress must be properly aligned or screen will be black!
    col *= pcs.anim_progress;

    outColor = vec4(col, 1.0);
}
