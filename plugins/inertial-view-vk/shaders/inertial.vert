#version 450

/**
 * @file inertial.vert
 * @brief Fullscreen triangle generator for post-processing.
 */

layout(location = 0) out vec2 v_uv;

void main() {
    /**
     * Vertex Index Generation Logic:
     * Index 0: v_uv = (0, 0), gl_Position = (-1, -1, 0, 1)
     * Index 1: v_uv = (2, 0), gl_Position = ( 3, -1, 0, 1)
     * Index 2: v_uv = (0, 2), gl_Position = (-1,  3, 0, 1)
     */
    v_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}
