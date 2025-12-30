#version 450

layout(location = 0) out vec2 uvpos;

void main() {
    // Generate UVs: (0,0), (2,0), (0,2)
    uvpos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);

    // Convert UVs to NDC: (-1,-1), (3,-1), (-1,3)
    // This ensures a Counter-Clockwise (CCW) winding order
    gl_Position = vec4(uvpos * 2.0f - 1.0f, 0.0f, 1.0f);

    // Standard Vulkan Y-flip is not needed here if your fragment shader 
    // expects top-left at (0,0)
}
