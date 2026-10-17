#version 450
layout(triangles) in;
layout(triangle_strip,max_vertices=3) out;
layout(location=0) noperspective in vec4 inUV[];
layout(location=1) flat in vec4 inLayer[];
layout(location=0) noperspective out vec4 uv;
layout(location=1) flat out vec4 layer;
void main(){for(int i=0;i<3;i++){gl_Position=gl_in[i].gl_Position;uv=inUV[i];layer=inLayer[0];gl_Layer=int(floatBitsToUint(layer.x));EmitVertex();}EndPrimitive();}
