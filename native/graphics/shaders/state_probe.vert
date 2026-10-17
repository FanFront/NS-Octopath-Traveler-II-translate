#version 450
layout(set=0,binding=1,std140) uniform State { vec4 color; vec4 params; } p;
void main(){vec2 v=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(v*2.0-1.0,p.params.x,1);}
