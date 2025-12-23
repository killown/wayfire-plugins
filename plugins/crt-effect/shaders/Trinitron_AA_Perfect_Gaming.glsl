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

// FD Trinitron Geometry (Strictly for the flat-screen FV310 series)
const float CURVE_X = 0.012;
const float CURVE_Y = 0.015;

vec2 curve(vec2 uv) {
    if (!distort_enable) return uv;
    uv = (uv - 0.5) * 2.0;
    uv *= 1.015; // Overscan to hide edge artifacts
    uv.x *= 1.0 + pow((abs(uv.y) * CURVE_X), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) * CURVE_Y), 2.0);
    uv = (uv / 2.0) + 0.5;
    return uv;
}

// Anti-Aliasing Logic: Gaussian Beam Spot size simulation
vec3 gaussian_sample(vec2 uv) {
    vec2 one_pixel = vec2(1.0) / resolution;
    // Weighted sample to melt digital edges without losing detail
    vec3 color = texture2D(texture, uv).rgb * 0.5;
    color += texture2D(texture, uv + vec2(one_pixel.x * 0.7, 0.0)).rgb * 0.25;
    color += texture2D(texture, uv - vec2(one_pixel.x * 0.7, 0.0)).rgb * 0.25;
    return color;
}

void main() {
    // 1. Geometry Correction
    vec2 curved_uv = curve(uvpos);
    if (curved_uv.x < 0.0 || curved_uv.x > 1.0 || curved_uv.y < 0.0 || curved_uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // 2. Chromatic Aberration + Gaussian AA
    vec3 col;
    if (aberration_enable) {
        float spread = 0.0007; // High-end Sony convergence is tight
        col.r = gaussian_sample(vec2(curved_uv.x + spread, curved_uv.y)).r;
        col.g = gaussian_sample(curved_uv).g;
        col.b = gaussian_sample(vec2(curved_uv.x - spread, curved_uv.y)).b;
    } else {
        col = gaussian_sample(curved_uv);
    }

    // 3. Scanlines (The "Underline" Slat Look)
    if (scanlines_enable) {
        // Density: 0.5 = 1 scanline pair per 2 monitor pixels
        float count = resolution.y * 0.5;
        
        // Subtle Animation: The slow crawl visible in your reference images
        float rolling_phase = time * 0.12;
        float sl = sin(uvpos.y * count * 6.28318 + rolling_phase);
        float scan_norm = (sl * 0.5) + 0.5;
        
        // SHARPEN: Power 1.4 creates the distinct horizontal black gaps from the photo
        scan_norm = pow(scan_norm, 1.4);

        // BLOOM: Bright pixels burn through the scanlines (0.45 strength)
        float lum = dot(col, vec3(0.299, 0.587, 0.114));
        float scan_visibility = 1.0 - (lum * 0.45); 
        
        // CONTRAST: mix(0.05, ...) ensures dark/black gaps like a real CRT
        // mix(..., 1.25) boosts the peak brightness of the beam center
        float line_intensity = mix(0.05, 1.25, scan_norm);
        col *= mix(1.0, line_intensity, scan_visibility);
    }

    // 4. Phosphor Mesh (The vertical Aperture Grille texture)
    if (scanlines_enable) {
        float mesh = sin(uvpos.x * resolution.x * 1.0 * 3.14159);
        float mesh_norm = (mesh * 0.5) + 0.5;
        // Subtle "Velvet" texture typical of Sony Grilles
        col *= mix(0.94, 1.0, mesh_norm);
    }

    // 5. Vignette (Natural Tube Light Falloff)
    if (vignette_enable) {
        float vig = uvpos.x * uvpos.y * (1.0 - uvpos.x) * (1.0 - uvpos.y);
        col *= pow(vig * 16.0, 0.06);
    }

    // 6. BEAUTIFY (Final Color Science for FV310)
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    vec3 gray = vec3(lum);
    
    // SATURATION: 1.3x boost to match the "Candy" look in your Yoshi reference
    col = mix(gray, col, 0.9);
    
    // FINAL GAIN & GAMMA: Compensation for scanline darkening
    col *= (0.3 + r_brightness); 
    col = pow(col, vec3(1.1)); // Deepens blacks to fix the "too bright" issue
    col *= anim_progress;

    gl_FragColor = vec4(col, 1.0);
}
