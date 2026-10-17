#version 450
layout(push_constant) uniform Clear { uvec4 color; float depth; } p;
layout(location=0) out uvec4 color;
void main() { color=p.color; }
