// ==UNIFORMS==
// time: true
// ==END_UNIFORMS==
precision highp float;
varying highp vec2 uvpos;
uniform sampler2D texture;
uniform vec2 resolution;
uniform float time;
uniform float anim_progress;
// Toggles from config
uniform bool distort_enable;
uniform bool scanlines_enable;
uniform bool vignette_enable;
uniform bool aberration_enable;
uniform float r_brightness;
// KV-24FS120 Geometry
const float CURVE_X = 0.012;
const float CURVE_Y = 0.015;
vec2 curve(vec2 uv) {
    if (!distort_enable) return uv;
    uv = (uv - 0.5) * 2.0;
    uv *= 1.01;
    uv.x *= 1.0 + pow((abs(uv.y) * CURVE_X), 20.0);
    uv.y *= 1.0 + pow((abs(uv.x) * CURVE_Y), 20.0);
    uv = (uv / 2.0) + 0.5;
    return uv;
}
vec3 gaussian_sample(vec2 uv) {
    float blur = 1.1 / resolution.x;
    vec3 color = texture2D(texture, uv).rgb * 0.4;
    color += texture2D(texture, uv + vec2(blur, 0.0)).rgb * 0.16;
    color += texture2D(texture, uv - vec2(blur, 0.0)).rgb * 0.16;
    return color;
}
void main() {
    vec2 curved_uv = curve(uvpos);
    if (curved_uv.x < 0.0 || curved_uv.x > 1.0 || curved_uv.y < 0.0 || curved_uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 col = gaussian_sample(curved_uv);
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    // 2. ROLLING BEAM SCANLINES
    if (scanlines_enable) {
        // A. Static Line Structure (Locked to grid)
        float count = resolution.y * 0.33;
        float sl = sin(uvpos.y * count * 6.28318);
        // B. Rolling Refresh Bar (The "One line at a time" feel)
        // This calculates a pulse that travels from top (0.0) to bottom (1.0)
        float scan_speed = 0.25;
        float beam_pos = fract(time * scan_speed);
        // Calculate distance from the rolling beam to the current pixel
        float dist = abs(uvpos.y - beam_pos);
        // Soften the beam width (0.1 is the 'glow' of the active scanning area)
        float beam_glow = smoothstep(0.1, 0.0, dist);
        // C. Slat Profile with Bloom
        float scan_norm = (sl * 0.5) + 0.5;
        float beam_profile = mix(1.8, 1.2, pow(lum, 1.5));
        scan_norm = pow(scan_norm, beam_profile);
        // D. Combine Static Lines with the Rolling Intensity
        float scan_visibility = 1.0 - (lum * 0.5);
        // Base lines are dark, but the 'beam_glow' adds back 15% brightness as it passes
        float line_intensity = mix(0.05, 1.1, scan_norm);
        line_intensity += beam_glow * 0.15 * lum;
        col *= mix(1.0, line_intensity, scan_visibility);
    }
    // 3. Aperture Grille
    if (scanlines_enable) {
        float mesh = sin(uvpos.x * resolution.x * 2.1);
        float mesh_norm = (mesh * 0.5) + 0.5;
        col *= mix(0.7, 1.0, mesh_norm);
    }
    // 4. Vignette
    if (vignette_enable) {
        float vig = uvpos.x * uvpos.y * (1.0 - uvpos.x) * (1.0 - uvpos.y);
        col *= pow(vig * 16.0, 0.06);
    }
    // 5. Final Color Tuning
    col *= (0.8 + r_brightness);
    float l = dot(col, vec3(0.299, 0.587, 0.114));
    col = mix(vec3(l), col, 1.12);
    col = pow(col, vec3(1.2));
    col *= anim_progress;
    gl_FragColor = vec4(col, 1.0);
}
