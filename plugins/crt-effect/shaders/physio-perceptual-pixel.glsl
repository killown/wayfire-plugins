// ==UNIFORMS==
// time: true
// ==END_UNIFORMS==
// Global precision declaration
precision highp float;
// Global Uniforms and Varyings
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform float anim_progress;
uniform float time;
// --- Brightness/Clarity Constants ---
const float BRIGHTNESS_LIFT = 0.75;        // Aggressive gamma boost for bright mid-tones
const float CHR_ABERRATION_AMOUNT = 0.001; // Subtle color bleeding magnitude
const float GLOBAL_DARK_COMPENSATION = 0.95; // Final uniform darkening
// --- Structural Constants (Near-Invisible) ---
const float SCANLINE_SUPPRESSION = 0.98;   // Near-Invisible 2% Scanline Darkening
const float SCANLINE_FREQUENCY = 4.0;      // Very thin lines
const float VERTICAL_MASK_DENSITY = 1.0;   // Single-pixel vertical line density
// --- Filmic Grain Constants (Chunky Flicker) ---
const float GRAIN_INTENSITY = 0.045;    // Heavy, visible grain
const float FBM_OCTAVES = 4.0;
const float FBM_LACUNARITY = 2.0;
const float FBM_GAIN = 0.5;
// --- Utility Functions: Base Hash ---
float hash11(float p) {
    p = fract(p * .1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}
// --- Fractal Brownian Motion (FBM) / Fractal Noise ---
float fbm(vec2 p) {
    float total_noise = 0.0;
    float amplitude = 1.0;
    float frequency = 1.0;
    for (int i = 0; i < int(FBM_OCTAVES); ++i) {
        float noise = hash11(p.x * frequency + p.y * frequency);
        total_noise += noise * amplitude;
        frequency *= FBM_LACUNARITY;
        amplitude *= FBM_GAIN;
    }
    return total_noise / (2.0 * amplitude);
}
// --- Aperture Grille Mask (Near-Invisible Vertical Structure) ---
vec3 trinitron_mask(vec2 uv) {
    // Single-pixel wide column for the vertical black line
    float density_x = resolution.x * VERTICAL_MASK_DENSITY;
    float col_index = mod(floor(uv.x * density_x), 4.0);
    vec3 mask = vec3(1.0); // Start with full brightness
    // Explicitly black out the 4th column (the vertical line)
    if (col_index > 2.9) {
        mask = vec3(0.0);
    }
    // Otherwise, apply R, G, B phosphor channels
    else if (col_index < 1.0) {
        mask.g = 0.0; mask.b = 0.0;
    } else if (col_index < 2.0) {
        mask.r = 0.0; mask.b = 0.0;
    } else {
        mask.r = 0.0; mask.g = 0.0;
    }
    return mask;
}
// --- Main Processing Function ---
vec3 process_retro(vec2 uv) {
    vec3 color = texture2D(smp, uv).rgb;
    // 1. Apply Aperture Grille Mask (Minimal Vertical Lines)
    vec3 mask = trinitron_mask(uv);
    color *= mask;
    // 2. Chromatic Aberration (Subtle color bleeding)
    vec3 aura_color;
    aura_color.r = texture2D(smp, uv + vec2(-CHR_ABERRATION_AMOUNT, 0.0)).r;
    aura_color.g = texture2D(smp, uv).g;
    aura_color.b = texture2D(smp, uv + vec2(CHR_ABERRATION_AMOUNT, 0.0)).b;
    color = mix(color, aura_color, 0.5);
    // 3. Minimal Desaturation
    float final_luma = dot(color, vec3(0.333));
    float desat_factor = pow(final_luma, 4.0) * 0.3;
    color = mix(color, vec3(final_luma), desat_factor);
    return color;
}
void main()
{
    vec2 uv = uvpos;
    // Safety cut-off
    if (uvpos.x < 0.0 || uvpos.x > 1.0 || uvpos.y < 0.0 || uvpos.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0) * anim_progress;
        return;
    }
    // Apply the main structural processing
    vec3 final_color = process_retro(uv);
    // 4. Apply Uniform Darkening
    final_color *= GLOBAL_DARK_COMPENSATION;
    // 5. Apply FRACTAL NOISE (Chunky, Flickering Grain)
    vec2 grain_coords = uv * resolution.x * 0.1 + vec2(0.0, time * 0.1);
    float fractal_noise_val = fbm(grain_coords);
    float grain_mix = (fractal_noise_val * 2.0 - 1.0) * GRAIN_INTENSITY + 1.0;
    final_color *= grain_mix;
    // 6. Near-Invisible Scanlines (Horizontal Lines)
    float scanline = sin(uv.y * resolution.y * SCANLINE_FREQUENCY) * 0.5 + 0.5;
    final_color *= mix(vec3(SCANLINE_SUPPRESSION), vec3(1.0), scanline);
    // 7. CRITICAL BRIGHTNESS LIFT
    final_color = pow(final_color, vec3(BRIGHTNESS_LIFT));
    // Final Clamp
    final_color = max(vec3(0.0), final_color);
    // Apply plugin fade
    gl_FragColor = vec4(final_color, 1.0) * anim_progress;
}
