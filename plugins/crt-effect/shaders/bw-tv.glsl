#version 100
precision highp float;

// Standard inputs provided by the plugin
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform float anim_progress;

// Universal toggles
uniform bool distort_enable;
uniform bool scanlines_enable;
uniform bool vignette_enable;
// Note: Chromatic aberration is ignored because B&W TVs don't have color!

// Standard curvature function
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.02, 1.02);
    uv.x *= 1.0 + pow((abs(uv.y) / 5.0), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 4.0), 2.0);
    return (uv / 2.0) + 0.5;
}

// Simple pseudo-random noise generator
float rand(vec2 co) {
    return fract(sin(dot(co.xy ,vec2(12.9898,78.233))) * 43758.5453);
}

void main() {
    vec2 uv = uvpos;
    if (distort_enable) uv = curve(uv);

    // Cutoff borders based on curvature
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Sample the desktop texture
    vec3 col = texture2D(smp, uv).rgb;

    // 1. Grayscale Conversion (Luma)
    // Using standard coefficients to convert RGB to grayscale luminance
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    col = vec3(lum);

    // 2. Vintage Contrast & Brightness crunch
    // Old TVs often crushed blacks and blew out whites
    col = (col - 0.5) * 1.3 + 0.5; // Increase contrast
    col *= 1.1; // boost brightness

    // 3. Analog Static/Grain Noise
    // Generate noise based on position and animation progress so it moves
    float noise = rand(uv * anim_progress);
    col += (noise - 0.5) * 0.12;

    // 4. Strong Retro Scanlines
    if (scanlines_enable) {
        // Simple sine wave scanlines, made slightly thicker/darker for an older feel
        float scan = sin(uv.y * resolution.y * 0.5 * 3.14159 * 2.0);
        col *= 1.0 - 0.3 * (0.5 - 0.5 * scan);
    }

    // 5. Heavy Vignette
    if (vignette_enable) {
        float vig = 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y);
        col *= pow(vig, 0.3);
    }

    // Apply open/close animation fade
    col *= anim_progress;

    gl_FragColor = vec4(col, 1.0);
}
