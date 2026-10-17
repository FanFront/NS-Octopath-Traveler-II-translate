#version 450
layout(set=0,binding=2,std140) uniform State { vec4 color; vec4 params; } p;
layout(location=0) out vec4 color;
void main(){color=p.color;}
