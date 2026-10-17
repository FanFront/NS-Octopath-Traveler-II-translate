#version 450
layout(push_constant) uniform Clear { ivec4 color; float depth; } p;
layout(location=0) out ivec4 color;
void main() { color=p.color; }
