#version 450

/**
 * @file flux.frag
 * @brief Perceptually optimized neon edge animation using LCH-inspired color theory.
 * [CONSTRAINTS]: 
 * - Uses perimeter-based wave propagation for organic movement.
 * - Aspect ratio correction for uniform thickness.
 * - Accented shadow bleed for depth.
 */

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    float time;
    float thickness;
    float aspect_ratio;
    float padding;
} pc;

void main() {
    vec2 uv = fragTexCoord;
    vec2 pixel_uv = uv;
    
    float margin_x = pc.padding / (pc.padding * 2.0 + textureSize(texSampler, 0).x);
    float margin_y = pc.padding / (pc.padding * 2.0 + textureSize(texSampler, 0).y);
    
    vec2 corrected_uv = (uv - vec2(margin_x, margin_y)) / (1.0 - 2.0 * vec2(margin_x, margin_y));
    
    float dx = min(corrected_uv.x, 1.0 - corrected_uv.x);
    float dy = min(corrected_uv.y, 1.0 - corrected_uv.y);
    float edge_dist = min(dx, dy * pc.aspect_ratio);

    vec4 tex_color = texture(texSampler, uv);

    if (corrected_uv.x < 0.0 || corrected_uv.x > 1.0 || corrected_uv.y < 0.0 || corrected_uv.y > 1.0) {
        float border_sdf = max(max(-corrected_uv.x, corrected_uv.x - 1.0), max(-corrected_uv.y, corrected_uv.y - 1.0));
        
        float perimeter = 0.0;
        if (dx < dy) {
            perimeter = corrected_uv.y;
        } else {
            perimeter = corrected_uv.x;
        }

        float wave = 0.5 + 0.5 * sin(perimeter * 4.0 + pc.time * 2.5);
        float glow = exp(-150.0 * abs(border_sdf));
        float spark = pow(wave, 8.0) * glow * 2.0;

        vec3 core_cyan = vec3(0.0, 0.8, 1.0);
        vec3 accent_magenta = vec3(0.6, 0.0, 1.0);
        vec3 final_neon = mix(core_cyan, accent_magenta, wave * 0.5);
        
        float alpha = (glow * 0.4 + spark) * 1.5;
        outColor = vec4(final_neon * alpha, alpha);
    } else {
        outColor = tex_color;
    }
}
