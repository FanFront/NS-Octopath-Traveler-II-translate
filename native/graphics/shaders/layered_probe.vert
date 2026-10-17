#version 450
layout(location=0) noperspective out vec4 uv;
layout(location=1) flat out vec4 layer;
void main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(p*2.0-1.0,0,1);uv=vec4(p,0,1);layer=vec4(uintBitsToFloat(uint(gl_InstanceIndex)),0,0,0);}
