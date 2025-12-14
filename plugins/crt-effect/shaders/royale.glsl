#version 100
precision highp float;
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform float anim_progress;
uniform int r_mask_type; 
uniform float r_beam_sigma;
uniform float r_border_size;
uniform float r_scanline_weight;
uniform float r_brightness; 
uniform vec2 r_convergence_x; 
uniform vec2 r_convergence_y;
uniform bool distort_enable;
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.0 + r_border_size); 
    uv.x *= 1.0 + pow((abs(uv.y) / 4.5), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 3.5), 2.0);
    return (uv / 2.0) + 0.5;
}
void main() {
    vec2 uv = uvpos;
    if (distort_enable) uv = curve(uv);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec2 uv_r = uv + vec2(r_convergence_x.x, r_convergence_y.x);
    vec2 uv_g = uv;
    vec2 uv_b = uv + vec2(r_convergence_x.y, r_convergence_y.y);
    vec3 col;
    col.r = texture2D(smp, uv_r).r;
    col.g = texture2D(smp, uv_g).g;
    col.b = texture2D(smp, uv_b).b;
    col = pow(col, vec3(2.2));
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    float scan_pos = fract(uv.y * resolution.y * 0.5) - 0.5; 
    float sigma = r_beam_sigma + (lum * 0.1); 
    float beam = exp(-(scan_pos * scan_pos) / (2.0 * sigma * sigma));
    col *= 1.0 - (r_scanline_weight * (1.0 - beam));
    float px = uv.x * resolution.x;
    float py = uv.y * resolution.y;
    float mask = 1.0;
    if (r_mask_type == 0) mask = 0.85 + 0.15 * sin(px * 3.14159 * 1.5); 
    else if (r_mask_type == 1) {
        float odd = mod(floor(py / 4.0), 2.0); 
        float slot = sin((px + odd * 1.5) * 3.14159 * 1.5);
        float vert = sin(py * 3.14159);
        mask = 0.8 + 0.2 * (slot * vert);
    } else mask = 0.8 + 0.2 * (sin(px * 3.0) * sin(py * 3.0));
    col *= mask;
    col = pow(col, vec3(1.0 / 2.2)); 
    col *= r_brightness; 
    col *= anim_progress;
    gl_FragColor = vec4(col, 1.0);
}