#version 100
#extension GL_OES_standard_derivatives : enable
// ==UNIFORMS==
// time: true
// ==END_UNIFORMS==

// Global precision declaration applied to all floats and vectors
precision highp float;

// Global Uniforms and Varyings
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform float anim_progress;
uniform float time;

// Global Constants
const float DECAY_RATE = 0.98;
const float APERTURE_DENSITY = 0.33;
const float CHR_ABERRATION_BASE = 0.002;

// --- Noise Functions ---
float hash11(float p) {
    p = fract(p * .1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

// Unified Noise for stable flicker (position-based) and grain
float unified_noise(vec2 p, float t) {
    float flicker = sin(p.y * 10.0 + t * 5.0) * 0.005 + 1.0; 
    
    vec2 grain_p = p * 0.1;
    float grain = mix(
        mix(hash11(grain_p.x + grain_p.y * 10.0 + t), hash11(grain_p.x + 1.0 + grain_p.y * 10.0 + t), fract(grain_p.x)), 
        mix(hash11(grain_p.x + (grain_p.y + 1.0) * 10.0 + t), hash11(grain_p.x + 1.0 + (grain_p.y + 1.0) * 10.0 + t), fract(grain_p.x)), 
        fract(grain_p.y)
    ) * 0.04 + 0.98;
    
    return grain * flicker;
}

// --- Main Sampling Function ---
vec3 sample_integrated(vec2 uv, float t) {
    // 1. Decoupled Chromatic Sampling
    float ab_scale = (1.0 / resolution.x) * CHR_ABERRATION_BASE; 
    float jitter_x = hash11(t * 10.0) * 0.0001;
    
    // Sample channels individually
    vec3 color_r = texture2D(smp, uv + vec2(-ab_scale * 1.5 - jitter_x, 0.0)).rgb;
    vec3 color_g = texture2D(smp, uv + vec2(jitter_x, 0.0)).rgb;
    vec3 color_b = texture2D(smp, uv + vec2(ab_scale * 1.5 + jitter_x * 0.5, 0.0)).rgb;
    
    // 2. Luminance-Preserving Sampling (LPS) - SIMPLIFIED
    // Use average brightness for internal math
    float luma_avg = (dot(color_r, vec3(1.0)) + dot(color_g, vec3(1.0)) + dot(color_b, vec3(1.0))) / 9.0;

    // Reconstruct the color using individual channel contributions
    vec3 final_color;
    final_color.r = color_r.r;
    final_color.g = color_g.g;
    final_color.b = color_b.b;
    
    // 3. Phosphor Screen Pattern (No Luminance Weighting)
    vec2 screen_coord = uv * resolution * APERTURE_DENSITY;
    float mask_index = mod(screen_coord.x, 3.0);
    
    // Subpixel decay
    if (mask_index < 1.0) {
        final_color.gb *= DECAY_RATE;
    } else if (mask_index < 2.0) {
        final_color.rb *= DECAY_RATE;
    } else {
        final_color.rg *= DECAY_RATE;
    }

    // 4. White Point Normalization
    // Use simple total luma
    float final_luma = dot(final_color, vec3(0.333));
    float desat_factor = pow(final_luma, 4.0) * 0.5;
    final_color = mix(final_color, vec3(final_luma), desat_factor); 

    // **5. Global Brightness Lift (THE FIX for Darkness)**
    final_color *= 1.1; // Lift the midtones by 10%
    
    // 6. Maximum Uniform Glow
    final_color = pow(final_color, vec3(1.6));
    
    return final_color;
}

void main()
{
    vec2 uv = uvpos;
    
    // Safety cut-off
    if (uvpos.x < 0.0 || uvpos.x > 1.0 || uvpos.y < 0.0 || uvpos.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0) * anim_progress;
        return;
    }
    
    // Apply the integrated sampling
    vec3 final_color = sample_integrated(uv, time);
    
    // Apply unified noise/flicker (Luminance only)
    float noise_mix = unified_noise(uv * resolution.x, time);
    final_color *= noise_mix;
    
    // Scanlines (Minimal suppression)
    float scanline = sin(uv.y * resolution.y * 1.5) * 0.5 + 0.5;
    final_color *= mix(vec3(0.99), vec3(1.0), scanline);
    
    // FINAL FIX: Shadow Clamp
    final_color = max(vec3(0.0), final_color);

    // Apply plugin fade
    gl_FragColor = vec4(final_color, 1.0) * anim_progress;
}
