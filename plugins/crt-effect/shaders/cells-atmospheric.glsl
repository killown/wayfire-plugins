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
const float FILM_GRAIN_INTENSITY = 0.04; // 4% visible noise intensity
const float BLOOM_RADIANCE = 1.6;        // Strong bloom for light sources
const float TEAL_SHIFT_AMOUNT = 0.15;    // Magnitude of the color shift
// --- Utility Functions ---
// Standard hash for noise stability
float hash11(float p) {
    p = fract(p * .1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}
// Convert from Gamma (sRGB) to Linear Color Space (Accurate Light Model)
vec3 to_linear(vec3 color) {
    return pow(color, vec3(2.2));
}
// Convert from Linear back to Gamma (sRGB) Color Space
vec3 from_linear(vec3 color) {
    return pow(color, vec3(1.0 / 2.2));
}
// --- Film Grain Generator ---
float film_grain(vec2 p, float t) {
    // High-density space-time noise for cinematic grain
    vec2 grain_p = p * resolution.x * 0.005; // Density based on screen size
    float noise = hash11(grain_p.x + grain_p.y * 100.0 + t * 5.0);
    // Scale the noise from -1 to 1, then apply intensity
    return (noise * 2.0 - 1.0) * FILM_GRAIN_INTENSITY;
}
// --- Main Processing Function ---
vec3 process_atmospheric(vec2 uv, float t) {
    vec3 color_gamma = texture2D(smp, uv).rgb;
    // 1. Convert to Linear Space for Accurate Bloom
    vec3 color_linear = to_linear(color_gamma);
    // 2. Volumetric Bloom Effect (Strong radiance on light sources)
    color_linear = pow(color_linear, vec3(BLOOM_RADIANCE));
    // 3. Convert back to Gamma Space
    vec3 final_color = from_linear(color_linear);
    // 4. Dead Cells Color Grading (Shadows to Teal/Cyan)
    float luma = dot(final_color, vec3(0.299, 0.587, 0.114)); // BT.601 Luma
    // Calculate how much to shift based on how dark the pixel is (1.0 = pitch black)
    float shadow_shift = 1.0 - smoothstep(0.0, 0.5, luma);
    // Define the teal/cyan shift vector
    vec3 teal_vector = vec3(-0.1, 0.05, 0.1);
    // Apply the shift: less red, more green and blue (teal/cyan) in shadows
    final_color += teal_vector * shadow_shift * TEAL_SHIFT_AMOUNT;
    // 5. Final Shadow Clamp (Guarantees deep, graded black)
    final_color = max(vec3(0.0), final_color);
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
    vec3 final_color = process_atmospheric(uv, time);
    // 6. Apply Cinematic Film Grain (Applied additively to prevent crushing)
    float grain = film_grain(uv * resolution.x, time);
    final_color += grain;
    // Final shadow clamp after grain
    final_color = max(vec3(0.0), final_color);
    // Apply plugin fade
    gl_FragColor = vec4(final_color, 1.0) * anim_progress;
}
