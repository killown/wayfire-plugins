#version 100
precision highp float;
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform float anim_progress;
uniform bool distort_enable;
uniform float r_brightness;
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.005, 1.005); 
    uv.x *= 1.0 + pow((abs(uv.y) / 6.0), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 5.0), 2.0);
    return (uv / 2.0) + 0.5;
}
vec3 vibrance(vec3 col, float val) {
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    vec3 mask = (col - vec3(lum));
    mask = clamp(mask, 0.0, 1.0);
    float lum_mask = 1.0 - dot(mask, vec3(0.299, 0.587, 0.114));
    lum_mask = 1.0 - pow(lum_mask, 3.0);
    return mix(col, vec3(lum), -val * lum_mask);
}
void main() {
    vec2 uv = uvpos;
    if (distort_enable) uv = curve(uv);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    vec3 col = texture2D(smp, uv).rgb;
    col = pow(col, vec3(2.2));
    float lum = dot(col, vec3(0.299, 0.587, 0.114));
    float scan_phase = sin(uv.y * resolution.y * 3.14159); 
    float scan_strength = 0.3 * (1.0 - lum); 
    col *= 1.0 - (scan_strength * (0.5 - 0.5 * scan_phase));
    float grill = sin(uv.x * resolution.x * 3.14159 * 1.5);
    col *= 1.0 - (0.15 * (0.5 - 0.5 * grill));
    col = pow(col, vec3(1.0 / 2.2)); 
    col = vibrance(col, 0.15); 
    col *= r_brightness; 
    col *= anim_progress;
    gl_FragColor = vec4(col, 1.0);
}