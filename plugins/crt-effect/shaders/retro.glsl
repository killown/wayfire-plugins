#version 100
precision highp float;
varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform highp float time; 
uniform bool distort_enable;
uniform float anim_progress;
vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= 1.1;    
    uv.x *= 1.0 + pow((abs(uv.y) / 5.0), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 4.0), 2.0);
    uv  = (uv / 2.0) + 0.5;
    uv =  uv * 0.92 + 0.04;
    return uv;
}
void main() {
    vec2 uv = uvpos;
    if (distort_enable) uv = curve(uv);
    vec3 col;
    float x = sin(0.3 * time + uv.y * 21.0) * sin(0.7 * time + uv.y * 29.0) * sin(0.3 + 0.33 * time + uv.y * 31.0) * 0.0017;
    col.r = texture2D(smp, vec2(x + uv.x + 0.001, uv.y + 0.001)).x + 0.05;
    col.g = texture2D(smp, vec2(x + uv.x + 0.000, uv.y - 0.002)).y + 0.05;
    col.b = texture2D(smp, vec2(x + uv.x - 0.002, uv.y + 0.000)).z + 0.05;
    float vig = (0.0 + 1.0 * 16.0 * uv.x * uv.y * (1.0 - uv.x) * (1.0 - uv.y));
    col *= vec3(pow(vig, 0.3));
    col *= vec3(0.95, 1.05, 0.95) * 2.8;
    float scans = clamp(0.35 + 0.35 * sin(3.5 * time + uv.y * resolution.y * 1.5), 0.0, 1.0);
    float s = pow(scans, 1.7);
    col = col * vec3(0.4 + 0.7 * s);
    if (distort_enable) {
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) col *= 0.0;
    }
    col *= anim_progress;
    gl_FragColor = vec4(col, 1.0);
}