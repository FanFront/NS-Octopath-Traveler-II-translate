#version 450
layout(push_constant) uniform Clear { vec4 color; float depth; } p;
layout(location=0) out vec4 color;
void main() { color=p.color; }
