#version 100
precision highp float;
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform bool distort_enable;
uniform bool scanlines_enable;
uniform bool vignette_enable;
uniform bool aberration_enable;
uniform float anim_progress;
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.02, 1.02);
    uv.x *= 1.0 + pow((abs(uv.y) / 5.0), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 4.0), 2.0);
    return (uv / 2.0) + 0.5;
}
void main() {
    vec2 flat_uv = uvpos;
    vec2 curved_uv = flat_uv;
    if (distort_enable) curved_uv = curve(flat_uv);
    vec2 uv = mix(flat_uv, curved_uv, anim_progress);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 color;
    if (aberration_enable) {
        float offset = 0.001 * anim_progress; 
        color.r = texture2D(smp, uv + vec2(offset, 0.0)).r;
        color.g = texture2D(smp, uv).g;
        color.b = texture2D(smp, uv - vec2(offset, 0.0)).b;
    } else {
        color = texture2D(smp, uv).rgb;
    }
    if (scanlines_enable) {
        float scan_cnt = resolution.y * 0.5;
        float scan = sin(uv.y * scan_cnt * 3.14159 * 2.0);
        color *= 1.0 - (0.15 * anim_progress) * (0.5 - 0.5 * scan);
        float grill = sin(uv.x * resolution.x * 0.333 * 3.14159 * 2.0);
        color *= 1.0 - (0.1 * anim_progress) * (0.5 - 0.5 * grill);
    }
    if (vignette_enable) {
        float vig = 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y);
        color *= mix(1.0, pow(vig, 0.15), anim_progress);
    }
    color *= 1.0 + (0.2 * anim_progress);
    gl_FragColor = vec4(color, 1.0);
}