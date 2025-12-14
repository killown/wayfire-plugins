#version 100
precision highp float;
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform float anim_progress;
uniform bool distort_enable;
uniform float r_brightness; 
const float HARD_SCAN = -8.0; 
const vec3 MASK_DARK = vec3(0.5, 0.5, 0.5);
const vec3 MASK_LIGHT = vec3(1.2, 1.2, 1.2);
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.01, 1.01);
    uv.x *= 1.0 + pow((abs(uv.y) / 4.8), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 3.8), 2.0);
    return (uv / 2.0) + 0.5;
}
vec4 get_tex_smart(sampler2D s, vec2 uv, vec2 res) {
    vec2 p = uv * res;
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f); 
    vec2 uv_smart = (i + f + 0.5) / res;
    return texture2D(s, uv_smart);
}
float scanline_gaussian(float uv_y, float res_y) {
    float scan_pos = fract(uv_y * res_y * 0.5);
    float dist = scan_pos - 0.5;
    return exp(HARD_SCAN * dist * dist);
}
vec3 slot_mask(vec2 pos) {
    float px = pos.x;
    float py = pos.y;
    float row = floor(py / 3.0); 
    float stagger = mod(row, 2.0) * 1.5; 
    float mask_val = sin((px + stagger) * 3.14159 * 1.2);
    mask_val = smoothstep(-0.5, 0.5, mask_val);
    return mix(MASK_DARK, MASK_LIGHT, mask_val);
}
void main() {
    vec2 uv = uvpos;
    if (distort_enable) uv = curve(uv);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 col = get_tex_smart(smp, uv, resolution).rgb;
    col = pow(col, vec3(2.4)); 
    float scan = scanline_gaussian(uv.y, resolution.y);
    col *= mix(0.6, 1.1, scan);
    vec3 mask = slot_mask(uv * resolution);
    col *= mask;
    col = pow(col, vec3(1.0 / 2.2));
    col *= 1.15 * r_brightness; 
    col *= anim_progress;
    gl_FragColor = vec4(col, 1.0);
}