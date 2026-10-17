#version 450
layout(push_constant) uniform Clear { vec4 color; float depth; } p;
void main() {
    vec2 position=vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position=vec4(position*2.0-1.0,p.depth,1.0);
}
