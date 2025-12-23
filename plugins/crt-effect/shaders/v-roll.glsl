#version 100
// ==UNIFORMS==
// time: true
// ==END_UNIFORMS==
precision highp float;

varying highp vec2 uvpos;
uniform sampler2D smp;
uniform highp vec2 resolution;
uniform float anim_progress;

uniform float time;
uniform bool distort_enable;

vec2 curve(vec2 uv) {
    uv = (uv - 0.5) * 2.0;
    uv *= vec2(1.02, 1.02);
    uv.x *= 1.0 + pow((abs(uv.y) / 5.0), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 4.0), 2.0);
    return (uv / 2.0) + 0.5;
}

void main()
{
    vec2 uv = uvpos;
    if (distort_enable) uv = curve(uv);

    vec4 bg = texture2D(smp, uv);
    float numCols = resolution.y / 8.0;
    float cell_h = 1.0 / numCols;
    float intensity = 0.25;
    float speed = 0.05;
    float offset = mod(time * speed, 1.0);
    float y_scaled = uv.y * resolution.y;
    float roll_pos = mod(y_scaled + offset * resolution.y, resolution.y);
    float localProgress = mod(roll_pos, numCols) / numCols;
    vec4 newcol = vec4(0.0, 0.0, 0.6, 0.0);
    float band_alpha = smoothstep(0.4, 0.6, localProgress);
    newcol.a = band_alpha * intensity;
    vec4 col = mix(bg, bg * vec4(0.5, 0.5, 0.5, 1.0), newcol.a);
    gl_FragColor = vec4(col.rgb, 1.0) * anim_progress;
}
