#version 450

layout(push_constant) uniform PushConstants {
    vec2 anchor_pos;
    vec2 velocity;
    float spring_k;
    float time;
    float aspect_ratio;
    float margin;
} pc;

layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

#define S(a, b, t) smoothstep(a, b, t)

vec3 N13(float p) {
   vec3 p3 = fract(vec3(p) * vec3(.1031,.11369,.13787));
   p3 += dot(p3, p3.yzx + 19.19);
   return fract(vec3((p3.x + p3.y)*p3.z, (p3.x+p3.z)*p3.y, (p3.y+p3.z)*p3.x));
}

float N(float t) {
    return fract(sin(t*12345.564)*7658.76);
}

float Saw(float b, float t) {
    return S(0., b, t)*S(1., b, t);
}

vec2 DropLayer(vec2 uv_in, float t) {
    vec2 UV = uv_in;
 
    uv_in.y -= t * 0.75; 
 
    vec2 a = vec2(6., 1.);
    vec2 grid = a * 2.;
    vec2 id = floor(uv_in * grid);

    float colShift = N(id.x); 
    uv_in.y += colShift;
 
    id = floor(uv_in * grid);
    vec3 n = N13(id.x * 35.2 + id.y * 2376.1);
    vec2 st = fract(uv_in * grid) - vec2(.5, 0);
 
    float x = n.x - .5;
    float y = UV.y * 20.;
    float wiggle = sin(y + sin(y));
    x += wiggle * (.5 - abs(x)) * (n.z - .5);
    x *= .7;
 
    float ti = fract(t + n.z);
    // Oscillate drop position within the cell
    y = (Saw(.85, ti) - .5) * .9 + .5;
    vec2 p = vec2(x, y);
 
    float d = length((st - p) * a.yx);
    float mainDrop = S(.4, .0, d);
 
    float r = sqrt(S(1., y, st.y));
    float cd = abs(st.x - x);
    float trail = S(.23 * r, .15 * r * r, cd);
    float trailFront = S(-.02, .02, st.y - y);
    trail *= trailFront * r * r;
 
    y = UV.y;
    float trail2 = S(.2 * r, .0, cd);
    float droplets = max(0., (sin(y * (1. - y) * 120.) - st.y)) * trail2 * trailFront * n.z;
    y = fract(y * 10.) + (st.y - .5);
    float dd = length(st - vec2(x, y));
    droplets = S(.3, 0., dd);
 
    float m = mainDrop + droplets * r * trailFront;
    return vec2(m, trail);
}

float StaticDrops(vec2 uv_in, float t) {
    uv_in *= 40.;
    vec2 id = floor(uv_in);
    uv_in = fract(uv_in) - .5;
    vec3 n = N13(id.x * 107.45 + id.y * 3543.654);
    vec2 p = (n.xy - .5) * .7;
    float d = length(uv_in - p);
    float fade = Saw(.025, fract(t + n.z));
    return S(.3, 0., d) * fract(n.z * 10.) * fade;
}

vec2 Drops(vec2 uv_in, float t, float l0, float l1, float l2) {
    float s = StaticDrops(uv_in, t) * l0; 
    vec2 m1 = DropLayer(uv_in, t) * l1;
    vec2 m2 = DropLayer(uv_in * 1.85, t) * l2;
 
    float c = s + m1.x + m2.x;
    c = S(.3, 1., c);
    return vec2(c, max(m1.y * l0, m2.y * l1));
}

void main() {
    vec2 p = (uv - 0.5);
    p.x *= pc.aspect_ratio;
 
    // Wind tilt: rain reacts to horizontal movement
    p.x += pc.velocity.x * 0.05 * p.y;

    float t = pc.time * 0.2;
    float rainAmount = 0.8;

    // Dynamic focus based on window state
    float maxBlur = 4.0;
    float minBlur = 0.5;

    float staticDrops = S(-.5, 1., rainAmount) * 2.5;
    float layer1 = S(.25, .75, rainAmount);
    float layer2 = S(.0, .5, rainAmount);
 
    vec2 c = Drops(p, t, staticDrops, layer1, layer2);
 
    // Calculate normals for refraction
    vec2 e = vec2(.001, 0.);
    float cx = Drops(p + e, t, staticDrops, layer1, layer2).x;
    float cy = Drops(p + e.yx, t, staticDrops, layer1, layer2).x;
    vec2 n = vec2(cx - c.x, cy - c.x); 

    // Focus calculation: trails cut through the fog
    float focus = mix(maxBlur - (c.y * maxBlur), minBlur, S(.1, .2, c.x));
 
    vec3 col = textureLod(tex, uv + n, focus).rgb;

    // Post-processing
    col *= vec3(0.9, 0.95, 1.1); // Cold tone
 
    // Lightning
    float lt = (pc.time + 3.0) * 0.5;
    float lightning = sin(lt * sin(lt * 10.0));
    lightning *= pow(max(0.0, sin(lt + sin(lt))), 10.0);
    col *= 1.0 + (lightning * 0.4);

    outColor = vec4(col, texture(tex, uv).a);
}
